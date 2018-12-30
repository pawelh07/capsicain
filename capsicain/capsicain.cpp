#include "pch.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>

#include <string>
#include <Windows.h>  //for Sleep()

#include "capsicain.h"
#include "mappings.h"
#include "scancodes.h"

using namespace std;

string version = "29";

const bool START_AHK_ON_STARTUP = true;
const int MAX_KEYMACRO_LENGTH = 10000;  //for testing; in real life, 100 keys = 200 up/downs should be enough

const int DEFAULT_DELAY_SENDMACRO = 5;  //local needs ~1ms, Linux VM 5+ms, RDP fullscreen 10+ for 100% reliable keystroke detection
const int DELAY_FOR_AHK = 50;
const int WAIT_FOR_INTERLEAVED_KEYS_MS = 200;  //when caps_down, key_down, caps_up, key_up : some keys wait for the next event
const int CAPS_TAPPED_TIMEOUT_MS = 1000;    //"caps tapped" state disappears after some time
const unsigned short AHK_HOTKEY1 = SC_F14;  //this key triggers supporting AHK script
const unsigned short AHK_HOTKEY2 = SC_F15;
string scLabels[256]; // contains [01]="ESCAPE" instead of SC_ESCAPE 

struct ModifierCombo
{
	unsigned short key = SC_NOP;
	unsigned short modAnd = 0;
	unsigned short modNot = 0;
	unsigned short modNop = 0; //block these modifiers
	unsigned short modFor = 0; //forward these modifiers
	unsigned short modTap = 0;
	vector<Stroke> strokeSequence;
};
vector<ModifierCombo> modCombosPre;
vector<ModifierCombo> modCombosPost;

struct Mode
{
	bool debug = false;
	int  activeLayer = 2;
	int characterCreationMode = 0;
	int delayBetweenMacroKeysMS = DEFAULT_DELAY_SENDMACRO;  //AHK drops keys when they are sent too fast
	bool lbSlashShift = true;
	bool slashShift = true;
	bool flipZy = true;
	bool flipAltWinOnAppleKeyboards = true;
	bool backslashToAlt = true;
} mode;

struct Mapping
{
	unsigned char alphamap[256] = { SC_NOP };
} mapping;

struct GlobalState
{
	bool keysDownReceived[256] = { false };
	bool keysDownSent[256] = { false };
	InterceptionContext interceptionContext = NULL;
	InterceptionDevice interceptionDevice = NULL;
	string deviceIdKeyboard = "";
	bool deviceIsAppleKeyboard = false;
	unsigned short bufferedScancode = 0; //hack to evaluate interleaved combos
	vector<unsigned short> oldmodsTempReleased; //remember modifiers that were released before playing macros
	unsigned short modsTempPressed; //remember modifiers that were altered before playing macros
	unsigned short modsTempReleased;
	vector<Stroke> modsTempAltered;
	Stroke previousStroke = { SC_NOP, 0 };
} globalState;

struct LoopState
{
	unsigned short scancode = 0;
	bool isDownstroke = false;
	InterceptionKeyStroke originalIKstroke = { SC_NOP, 0 };
	Stroke originalStroke = { SC_NOP, false };

	unsigned short modifiers = 0;
	bool blockKey = false;  //true: do not send the current key
	bool isFinalScancode = false;  //true: don't remap the scancode anymore
	bool isCapsTapped = false;  //TODO get rid of it
	InterceptionKeyStroke keyMacro[MAX_KEYMACRO_LENGTH];  //TODO GET RID OF THIS
	int keyMacroLength = 0;  // >0 triggers sending of macro instead of scancode. MUST match the actual # of chars in keyMacro

	vector<Stroke> resultingStrokeSequence;
} state;

string errorLog = "";
void error(string txt)
{
	cout << endl << "ERROR: " << txt;
	errorLog += "\r\n" + txt;
}


void initAllStatesToDefault()
{
//	globalState.interceptionDevice = NULL;
	globalState.deviceIdKeyboard = "";
	globalState.deviceIsAppleKeyboard = false;
	globalState.oldmodsTempReleased.clear();

	globalState.previousStroke = { 0,0 };
	state.modifiers = 0;
	state.blockKey = false;  //true: do not send the current key
	state.isFinalScancode = false;  //true: don't remap the scancode anymore
	state.isDownstroke = false;
	state.keyMacroLength = 0;  // >0 triggers sending of macro instead of scancode. MUST match the actual # of chars in keyMacro
	state.isCapsTapped = false;
	mode.delayBetweenMacroKeysMS = DEFAULT_DELAY_SENDMACRO;

}

void resetCapsNumScrollLock()
{
	//set NumLock, release CapsLock+Scrolllock
	state.keyMacroLength = 0;
	if (GetKeyState(VK_CAPITAL) & 0x0001)
		macroMakeBreakKey(SC_CAPS);
	if (!(GetKeyState(VK_NUMLOCK) & 0x0001))
		macroMakeBreakKey(SC_NUMLOCK);
	if (GetKeyState(VK_SCROLL) & 0x0001)
		macroMakeBreakKey(SC_SCRLOCK);
	playMacro(state.keyMacro, state.keyMacroLength);
	state.keyMacroLength = 0;
}

void setupConsoleWindow()
{
	//disable quick edit; blocking the console window means the keyboard is dead
	HANDLE Handle = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode;
	GetConsoleMode(Handle, &mode);
	mode &= ~ENABLE_QUICK_EDIT_MODE;
	mode &= ~ENABLE_MOUSE_INPUT;
	SetConsoleMode(Handle, mode);

	system("color 8E");  //byte1=background, byte2=text
	string title = ("Capsicain v" + version);
	SetConsoleTitle(title.c_str());
}

//catch fast interleaved typed �
chrono::steady_clock::time_point capsDownTimestamp;

bool readIniFeatures()
{
	vector<string> iniLines; //sanitized content of the .ini file
	if (!parseConfig(iniLines))
	{
		return false;
	}
	mode.debug = configHasKey("DEFAULTS", "debug", iniLines);
	if (!configReadInt("DEFAULTS", "activeLayer", mode.activeLayer, iniLines))
		cout << "Invalid ini file: cannot read activeLayer";
	if (!configReadInt("DEFAULTS", "characterCreationMode", mode.characterCreationMode, iniLines))
		cout << "Invalid ini file: cannot read characterCreationMode";
	if (!configReadInt("DEFAULTS", "delayBetweenMacroKeysMS", mode.delayBetweenMacroKeysMS, iniLines))
		cout << "Invalid ini file: cannot read delayBetweenMacroKeysMS";
	mode.lbSlashShift = configHasKey("FEATURES", "backslashShift", iniLines);
	mode.slashShift = configHasKey("FEATURES", "slashShift", iniLines);
	mode.flipZy = configHasKey("FEATURES", "flipZy", iniLines);
	mode.flipAltWinOnAppleKeyboards = configHasKey("FEATURES", "flipAltWinOnAppleKeyboards", iniLines);
	return true;
}

void resetAlphaMap()
{
	for (int i = 0; i < 256; i++)
		mapping.alphamap[i] = i;
}

bool readIniAlphaMappingLayer(int layer)
{
	vector<string> iniLines; //sanitized content of the .ini file
	if (!parseConfigSection("LAYER" + std::to_string(layer), iniLines))
	{
		IFDEBUG cout << endl << "No mapping defined for layer " << layer << endl;
		return true;
	}

	resetAlphaMap();

	for (string line : iniLines)
	{
		string a = stringToUpper(stringGetFirstToken(line));
		string b = stringToUpper(stringGetLastToken(line));
		unsigned char from = getScancode(a, scLabels);
		unsigned char to = getScancode(b, scLabels);
		if ((from == 0 && a != "NOP") || (to == 0 && b != "NOP"))
		{
			cout << endl << "Error in .ini [LAYER" << layer << "] : " << line;
			return false;
		}
		mapping.alphamap[from] = to;
	}
	return true;
}

bool readIniModCombosPre()
{
	vector<string> iniLines; //sanitized content of the .ini file
	if (!parseConfigSection("LAYER_ALL_PRE", iniLines))
	{
		IFDEBUG cout << endl << "No mapping defined for pre modifier combos." << endl;
		return true;
	}
	modCombosPre.clear();
	unsigned short mods[5] = { 0 }; //and, not, nop, for, tap
	vector<Stroke> strokeSequence;

	for (string line : iniLines)
	{
		unsigned short key;
		if (parseModCombo(line, key, mods, strokeSequence, scLabels))
		{
			IFDEBUG cout << endl << "modComboPre: " << line << endl << "    ," << key << " -> " 
				<< mods[0] << "," << mods[1] << "," << mods[1] << "," << mods[2] << "," << mods[3] << "," << mods[4] << "," << "sequence:" << strokeSequence.size();
			modCombosPre.push_back({ key, mods[0], mods[1], mods[2], mods[3], mods[4], strokeSequence });
		}
		else
		{
			cout << endl << "Error in .ini: cannot parse: " << line;
			return false;
		}
	}
	return true;
}

bool readIni()
{
	if (!readIniFeatures())
		return false;

	vector<ModifierCombo> oldModCombos(modCombosPre);
	if (!readIniModCombosPre())
	{
		modCombosPre = oldModCombos;
		cout << endl << "Cannot read modifier combos (pre). Keeping old combos." << endl;
	}

	unsigned char oldAlphamap[256];
	std::copy(mapping.alphamap, mapping.alphamap + 256, oldAlphamap);
	if (!readIniAlphaMappingLayer(mode.activeLayer))
	{
		std::copy(oldAlphamap, oldAlphamap + 256, mapping.alphamap);
		cout << endl << "Cannot read alpha mapping from ini. Keeping old alphamap." << endl;
	}
	return true;
}

int main()
{
	setupConsoleWindow();
	printHelloHeader();
	initScancodeLabels(scLabels);
	initAllStatesToDefault();
	resetAlphaMap();

	if (!readIni())
	{
		cout << endl << "No capsicain.ini - exiting..." << endl;
		Sleep(5000);
		return 0;
	}

	Sleep(700); //time to release shortcut keys that started capsicain
	raise_process_priority(); //careful: if we spam key events, other processes get no timeslots to process them. Sleep a bit...

	globalState.interceptionContext = interception_create_context();
	interception_set_filter(globalState.interceptionContext, interception_is_keyboard, INTERCEPTION_FILTER_KEY_ALL);

	printHelloHelp();

	if (START_AHK_ON_STARTUP)
	{
		string msg = startProgramSameFolder(PROGRAM_NAME_AHK);
		cout << endl << endl << "starting AHK... ";
		cout << (msg == "" ? "OK" : "Not. '" + msg + "'");
	}

	cout << endl << endl << "detecting keyboard (waiting for the first key)... ";

	while (interception_receive(
		globalState.interceptionContext,
		globalState.interceptionDevice = interception_wait(globalState.interceptionContext),
		(InterceptionStroke *)&state.originalIKstroke, 1) > 0
		)
	{
		//IFDEBUG cout << endl << "{{" << state.stroke.code << "|" << state.stroke.state << "}}";

		state.keyMacroLength = 0;
		state.blockKey = false;  //true: do not send the current key
		state.isFinalScancode = false;  //true: don't remap the scancode anymore
		state.isDownstroke = false;
		state.resultingStrokeSequence.clear();
		state.originalStroke = ikstroke2stroke(state.originalIKstroke);

		//check device ID
		if (globalState.deviceIdKeyboard.length() < 2)
		{
			getHardwareId();
			cout << endl << (globalState.deviceIsAppleKeyboard ? "Apple keyboard" : "IBM keyboard");
			resetCapsNumScrollLock();
			cout << endl << endl << "capsicain running...";
		}

		//evaluate and normalize the stroke         

		state.scancode = state.originalStroke.scancode;  //scancode will be altered, stroke won't
		state.isDownstroke = state.originalStroke.isDownstroke;
		if (state.originalIKstroke.code >= 0x80)
		{
			error("Received unexpected extended Interception Key Stroke code > 0x79: " + to_string(state.originalIKstroke.code));
			continue;
		}

		globalState.keysDownReceived[state.scancode] = state.isDownstroke;

		//command stroke: ESC + stroke
		// some major key shadowing here...
		// - cherry is good
		// - apple keyboard cannot do RCTRL+CAPS+ESC and Caps shadows the entire row a-s-d-f-g-....
		// - Dell cant do ctrl-caps-x
		// - Cypher has no RControl... :(
		// - HP shadows the 2-w-s-x and 3-e-d-c lines
		if (state.scancode == SC_ESCAPE)
		{
			//ESC alone will send ESC; otherwise block
			if (!state.isDownstroke && globalState.previousStroke.scancode == SC_ESCAPE)
			{
				IFDEBUG cout << " ESC ";
				sendStroke({ SC_ESCAPE, true });
				sendStroke({ SC_ESCAPE, false });
			}
			globalState.previousStroke = { state.scancode, state.isDownstroke };
			continue;
		}
		else
		{
			if (state.isDownstroke && globalState.keysDownReceived[SC_ESCAPE])
			{
				if (state.scancode == SC_X)
				{
					break;  //break the main while() loop, exit
				}
				else if (state.scancode == SC_Q) //stop the debug client while the release build keeps running
				{
#ifndef NDEBUG
					break;
#else
					sendStroke(globalState.previousStroke);
					sendStroke(state.stroke);
					continue;
#endif
				}
				else
				{
					processCommand();
					globalState.previousStroke = state.originalStroke;
					continue;
				}
			}
		}

		if (mode.activeLayer == 0)  //standard keyboard, just forward everything except command strokes
		{
			interception_send(globalState.interceptionContext, globalState.interceptionDevice, (InterceptionStroke *)&state.originalIKstroke, 1);
			continue;
		}

		/////CONFIGURED RULES//////
		IFDEBUG cout << endl << " [" << scLabels[state.scancode] << getSymbolForStrokeState(state.originalIKstroke.state) 
			<< " =" << hex << state.originalIKstroke.code << " " << state.originalIKstroke.state << "]";

		if (state.isCapsTapped && millisecondsSinceTimepoint(capsDownTimestamp) > CAPS_TAPPED_TIMEOUT_MS)
		{
			state.isCapsTapped = false;
			IFDEBUG cout <<  " [*Forgetting Caps tap. Was " << dec << millisecondsSinceTimepoint(capsDownTimestamp) << "ms ago] ";
		}

		//Catch fast typed interleaved caps+key. React to the previous key now that we know what came next.
		if (globalState.bufferedScancode != 0)
		{
			processBufferedScancode();
		}

		//Caps strokes: track but never forward 
		//if (state.scancode == SC_CAPS)
		//{
		//	processCaps();
		//	state.previousStroke = state.stroke;
		//	IFDEBUG cout << "\t--> BLOCKED";
		//	continue;
		//}

		//process the key stroke
		processRemapModifiers();
		processModifierState();
		IFDEBUG cout << " [" << hex << state.modifiers << (state.isCapsTapped ? "T" : "_") << "] ";

		//new rule based processing
		if (!state.isFinalScancode)
		{
			for (ModifierCombo modcombo : modCombosPre)
			{
				if (modcombo.key == state.scancode)
				{
					if (
						(state.modifiers & modcombo.modAnd) == modcombo.modAnd &&
						(state.modifiers & modcombo.modNot) == 0
						)
					{
						if(state.isDownstroke)
							state.resultingStrokeSequence = modcombo.strokeSequence;
						state.isFinalScancode = true;
						state.blockKey = true;
						break;
					}
				}
			}
		}

//		if (!state.isFinalScancode)
//			processLayoutIndependentAction();  //like caps+J

		if (!state.isFinalScancode && !IS_LCTRL_DOWN) //basic char layout. Don't remap the Ctrl combos?
		{
			processAlphaMappingTable(state.scancode);
			if (mode.flipZy)
				flipZY(state.scancode);
		}

		if (!state.isFinalScancode)
			processLayoutDependentActions();

		sendResultingKeyOrMacro();
		globalState.previousStroke = state.originalStroke;
	}
	interception_destroy_context(globalState.interceptionContext);

	cout << endl << "bye" << endl;
	return 0;
}
////////////////////////////////////END MAIN//////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////

void processAlphaMappingTable(unsigned short &scancode)
{
	if (scancode > 0xFF)
	{
		error("Unexpected scancode > 255 while mapping alphachars: " + std::to_string(scancode));
	}
	else
	{
		scancode = mapping.alphamap[scancode];
	}
}

void processBufferedScancode()
{
	IFDEBUG cout << " ***Processing a buffered scancode: " << hex << globalState.bufferedScancode;
	state.keyMacroLength = 0;
	switch (globalState.bufferedScancode)
	{
	case SC_A:
	{
		if (state.scancode == SC_A && !state.isDownstroke)
			createMacroKeyCombo(SC_LCTRL, SC_Z, 0, 0);
		else if (state.scancode == SC_CAPS && !state.isDownstroke)
			processCapsTapped(SC_A, mode.characterCreationMode);
		break;
	}
	case SC_O:
	{
		if (state.scancode == SC_O && !state.isDownstroke)
			macroMakeBreakKey(SC_PGUP);
		else if (state.scancode == SC_CAPS && !state.isDownstroke)
			processCapsTapped(SC_O, mode.characterCreationMode);
		break;
	}
	case SC_U:
	{
		if (state.scancode == SC_U && !state.isDownstroke)
			macroMakeBreakKey(SC_END);
		else if (state.scancode == SC_CAPS && !state.isDownstroke)
			processCapsTapped(SC_U, mode.characterCreationMode);
		break;
	}
	default:
		IFDEBUG cout << " ***buffered scancode discarded";
		break;
	}

	if (state.keyMacroLength > 0)
	{
		playMacro(state.keyMacro, state.keyMacroLength);
		state.keyMacroLength = 0;
	}
	globalState.bufferedScancode = 0;
}

void processCommand()
{
	cout << endl << endl << "::";

	switch (state.scancode)
	{
	case SC_0:
		mode.activeLayer = 0;
		cout << "LAYER 0: No changes at all (except core commands)";
		break;
	case SC_1:
	case SC_2:
	case SC_3:
	case SC_4:
	case SC_5:
	case SC_6:
	case SC_7:
	case SC_8:
	case SC_9:
	{
		int layer = state.scancode - 1;
		if (readIniAlphaMappingLayer(layer))
		{
			mode.activeLayer = layer;
			cout << "LAYER CHANGE: " << layer;
		}
		else
			cout << "LAYER IS NOT DEFINED";
		break;
	}
	case SC_R:
		cout << "RESET";
		reset();
		readIniFeatures();
		getHardwareId();
		cout << endl << (globalState.deviceIsAppleKeyboard ? "APPLE keyboard (flipping Win<>Alt)" : "PC keyboard");
		break;
	case SC_D:
		mode.debug = !mode.debug;
		cout << "DEBUG mode: " << (mode.debug ? "ON" : "OFF");
		break;
	case SC_SLASH:
		mode.slashShift = !mode.slashShift;
		cout << "Slash-Shift mode: " << (mode.slashShift ? "ON" : "OFF");
		break;
	case SC_LBSLASH:
		mode.lbSlashShift = !mode.lbSlashShift;
		cout << "Backslash-Shift mode: " << (mode.lbSlashShift ? "ON" : "OFF");
		break;
	case SC_Z:
		mode.flipZy = !mode.flipZy;
		cout << "Flip Z<>Y mode: " << (mode.flipZy ? "ON" : "OFF");
		break;
	case SC_W:
		mode.flipAltWinOnAppleKeyboards = !mode.flipAltWinOnAppleKeyboards;
		cout << "Flip ALT<>WIN mode: " << (mode.flipAltWinOnAppleKeyboards ? "ON" : "OFF") << endl;
		break;
	case SC_E:
		cout << "ERROR LOG: " << endl << errorLog << endl;
		break;
	case SC_S:
		printStatus();
		break;
	case SC_H:
		printHelp();
		break;
	case SC_C:
		cout << "Character creation mode: ";
		switch (mode.characterCreationMode)
		{
		case 0:
			mode.characterCreationMode = 1;
			cout << "ANSI (Alt + Numpad 0nnn)";
			break;
		case 1:
			mode.characterCreationMode = 2;
			cout << "AHK";
			break;
		case 2:
			mode.characterCreationMode = 0;
			cout << "IBM (Alt + Numpad nnn)";
			break;
		}
		cout << " (" << mode.characterCreationMode << ")";
		break;
	case SC_LBRACK:
		if (mode.delayBetweenMacroKeysMS >= 2)
			mode.delayBetweenMacroKeysMS -= 1;
		cout << "delay between characters in macros (ms): " << dec << mode.delayBetweenMacroKeysMS;
		break;
	case SC_RBRACK:
		if (mode.delayBetweenMacroKeysMS <= 100)
			mode.delayBetweenMacroKeysMS += 1;
		cout << "delay between characters in macros (ms): " << dec << mode.delayBetweenMacroKeysMS;
		break;
	case SC_A:
	{
		cout << "Start AHK";
		string msg = startProgramSameFolder("autohotkey.exe");
		if (msg != "")
			cout << endl << "Cannot start: " << msg;
		break;
	}
	case SC_K:
		cout << "Stop AHK";
		closeOrKillProgram("autohotkey.exe");
		break;
	default:
		cout << "Unknown command";
		break;
	}
}


void sendResultingKeyOrMacro()
{
	if (state.resultingStrokeSequence.size() > 0)
	{
		playStrokeSequence(state.resultingStrokeSequence);
	}
	else if (state.keyMacroLength > 0)
	{
		playMacro(state.keyMacro, state.keyMacroLength);
	}
	else
	{
		IFDEBUG
		{
			if (state.blockKey)
				cout << "\t--> BLOCKED ";
			else if (state.originalStroke.scancode != state.scancode)
				cout << "\t--> " << scLabels[state.scancode] << " " << getSymbolForStrokeState(state.originalIKstroke.state);
			else
				cout << "\t--";
		}
		if (!state.blockKey && !state.scancode == SC_NOP)
		{
			sendStroke({ state.scancode, state.isDownstroke });
		}
	}
}

void processLayoutIndependentAction()
{
	//TODO I BELIEVE THIS FUNCTION IS NOW OBSOLETE IN V2
	return;

	if (IS_CAPS_DOWN)
	{
		//those suppress the key UP because DOWN is replaced with macro
		bool blockingScancode = true;
		switch (state.scancode)
		{
			//Undo Redo Cut Copy Paste
		case SC_BACK:
			if (state.isDownstroke) {
				if (IS_SHIFT_DOWN)
					createMacroKeyComboRemoveShift(SC_LCTRL, SC_Y, 0, 0);
				else
					createMacroKeyCombo(SC_LCTRL, SC_Z, 0, 0);
			}
			break;
		case SC_A:
			if (state.isDownstroke)
			{
				if (millisecondsSinceTimepoint(capsDownTimestamp) > WAIT_FOR_INTERLEAVED_KEYS_MS)
				{
					IFDEBUG cout << "  ***long press: Caps A";
					createMacroKeyCombo(SC_LCTRL, SC_Z, 0, 0);
				}
				else
				{
					IFDEBUG cout << "  ???short press: Caps A? (" << dec << millisecondsSinceTimepoint(capsDownTimestamp) << "ms)" << " -> Buffering key...";
					globalState.bufferedScancode = state.scancode;
				}
			}
			break;
		case SC_S:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_LCTRL, SC_X, 0, 0);
			break;
		case SC_D:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_LCTRL, SC_C, 0, 0);
			break;
		case SC_F:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_LCTRL, SC_V, 0, 0);
			break;
		case SC_P:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_NUMLOCK, 0, 0, 0);
			break;
		case SC_LBRACK:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_SCRLOCK, 0, 0, 0);
			break;
		case SC_RBRACK:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_SYSRQ, 0, 0, 0);
			break;
		case SC_EQUALS:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_INSERT, 0, 0, 0);
			break;
		case SC_J:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_LEFT, 0, 0, 0, state.modifiers);
			break;
		case SC_L:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_RIGHT, 0, 0, 0, state.modifiers);
			break;
		case SC_K:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_DOWN, 0, 0, 0, state.modifiers);
			break;
		case SC_I:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_UP, 0, 0, 0, state.modifiers);
			break;
		case SC_O:
			if (state.isDownstroke)
			{
				if (millisecondsSinceTimepoint(capsDownTimestamp) > WAIT_FOR_INTERLEAVED_KEYS_MS)
				{
					IFDEBUG cout << "  ***Long press: Page Up";
					createMacroKeyCombo10timesIfAltDown(SC_PGUP, 0, 0, 0, state.modifiers);
				}
				else
				{
					IFDEBUG cout << "  ???Short press: Page Up? (" << dec << millisecondsSinceTimepoint(capsDownTimestamp) << "ms)" << " -> Buffering key...";
					globalState.bufferedScancode = state.scancode;
				}
			}
			break;
		case SC_SEMI:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_DELETE, 0, 0, 0);
			break;
		case SC_DOT:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_PGDOWN, 0, 0, 0, state.modifiers);
			break;
		case SC_Y:
			if (state.isDownstroke)
				createMacroKeyCombo(SC_HOME, 0, 0, 0);
			break;
		case SC_U:
			if (state.isDownstroke)
			{
				if (millisecondsSinceTimepoint(capsDownTimestamp) > WAIT_FOR_INTERLEAVED_KEYS_MS)
				{
					IFDEBUG cout << "  ***Long press: End";
					createMacroKeyCombo(SC_END, 0, 0, 0);
				}
				else
				{
					IFDEBUG cout << " ???Short press: End? (" << dec << millisecondsSinceTimepoint(capsDownTimestamp) << "ms)" << " -> Buffering key...";
					globalState.bufferedScancode = state.scancode;
				}
			}
			break;
		case SC_N:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_LCTRL, SC_LEFT, 0, 0, state.modifiers);
			break;
		case SC_M:
			if (state.isDownstroke)
				createMacroKeyCombo10timesIfAltDown(SC_LCTRL, SC_RIGHT, 0, 0, state.modifiers);
			break;
		case SC_0:
		case SC_1:
		case SC_2:
		case SC_3:
		case SC_4:
		case SC_5:
		case SC_6:
		case SC_7:
		case SC_8:
		case SC_9:
			if (state.isDownstroke)
				createMacroKeyCombo(AHK_HOTKEY1, state.scancode, 0, 0);
			break;
		case SC_T: //TEST
		{
			if (state.isDownstroke)
			{

			}
			break;
		}
		default:
			blockingScancode = false;
		}

		if (blockingScancode)
			state.blockKey = true;
		else  //direct key remapping without macros
		{
			switch (state.scancode)
			{
			case SC_H:
				state.scancode = SC_BACK;
				state.isFinalScancode = true;
				break;
			case SC_BSLASH:
				state.scancode = SC_SLASH;
				state.isFinalScancode = true;
				break;
			}
		}
	}
}

void processLayoutDependentActions()
{
	if (state.isCapsTapped) //so far, all caps tap actions are layout dependent (tap+A moves when A moves)
	{
		if (state.scancode != SC_LSHIFT && state.scancode != SC_RSHIFT)  //shift does not break the tapped status so I can type ��
		{
			if (state.isDownstroke)
			{
				processCapsTapped(state.scancode, mode.characterCreationMode);
			}
			if (state.keyMacroLength == 0)
			{
				state.isCapsTapped = false;
				state.blockKey = true;
			}
		}
	}
}


void processModifierState()
{
	if (!isModifier(state.scancode))
		return;

	unsigned short oldState = state.modifiers;
	unsigned short bitmask = getBitmaskForModifier(state.scancode);
	if (state.isDownstroke)
		state.modifiers |= bitmask;
	else 
		state.modifiers &= ~bitmask;

	state.isFinalScancode = true;
	if ((bitmask & 0xFF00) > 0)
		state.blockKey = true;

	bool wasTapped = !state.isDownstroke && (state.scancode == globalState.previousStroke.scancode);

	//special behaviour
	switch (state.scancode)
	{
	case SC_CAPS:  //timing for tapped chars
		if (state.isDownstroke)
		{
			state.isCapsTapped = false;
			if ((oldState & BITMASK_CAPS) == 0)  //initial down, not autorepeat
				capsDownTimestamp = timepointNow();
		}
		else
		{
			if (wasTapped)
			{
				state.isCapsTapped = true;
				IFDEBUG cout << " capsTap:" << (state.isCapsTapped ? "ON " : "OFF")
					<< " / TapTime = " << dec << millisecondsSinceTimepoint(capsDownTimestamp);
			}
		}
		break;
	case SC_LSHIFT:  //handle LShift+RShift -> CapsLock
		if (state.isDownstroke
			&& (state.modifiers == (BITMASK_LSHIFT | BITMASK_RSHIFT))
			&& (GetKeyState(VK_CAPITAL) & 0x0001)) //ask Win for Capslock state
		{
			macroMakeBreakKey(SC_CAPS);
		}
		break;
	case SC_RSHIFT:
		if (state.isDownstroke
			&& (state.modifiers == (BITMASK_LSHIFT | BITMASK_RSHIFT))
			&& !(GetKeyState(VK_CAPITAL) & 0x0001))
		{
			macroMakeBreakKey(SC_CAPS);
		}
		break;
	case SC_TAB:
		if (wasTapped)
			macroMakeBreakKey(SC_TAB);
		break;
	}
}

void processRemapModifiers()
{
	state.isFinalScancode = true;

	if (mode.flipAltWinOnAppleKeyboards && globalState.deviceIsAppleKeyboard)
	{
		switch (state.scancode)
		{
		case SC_LALT: state.scancode = SC_LWIN; break;
		case SC_LWIN: state.scancode = SC_LALT; break;
		case SC_RALT: state.scancode = SC_RWIN; break;
		case SC_RWIN: state.scancode = SC_RALT; break;
		}
	}

	switch (state.scancode)
	{
	case SC_LBSLASH:
		if (mode.lbSlashShift)
			state.scancode = SC_LSHIFT;
		break;
	case SC_SLASH:
		if (mode.slashShift && !IS_CAPS_DOWN)
			state.scancode = SC_RSHIFT;
		break;
	case SC_LALT:
		if (mode.backslashToAlt)
			state.scancode = SC_MOD12;
		break;
	case SC_RALT:
		if (mode.backslashToAlt)
			state.scancode = SC_MOD12;
		break;
	case SC_BSLASH:
		if (mode.backslashToAlt)
			state.scancode = SC_RALT;
	default:
		state.isFinalScancode = false;
	}
}

//May contain Capsicain Escape sequences, those are a bit hacky. 
//They are used to temporarily make/break modifiers (for example, ALT+Q -> Shift+1... but don't mess with shift state if it is actually pressed)
//Sequence starts with SC_CPS_ESC DOWN
//Scancodes inside are modifier bitmasks. State DOWN means "set these modifiers if they are up", UP means "clear those if they are down".
//Sequence ends with SC_CPS_ESC UP -> the modifier sequence is played.
//Second SC_CPS_ESC UP -> the previous changes to the modifiers are reverted.
void playStrokeSequence(vector<Stroke> strokeSequence)
{
	Stroke newstroke;
	unsigned int delay = mode.delayBetweenMacroKeysMS;
	bool inCpsEscape = false;  //inside an escape sequence, read next stroke

	IFDEBUG cout << "\t--> PLAY STROKE SEQUENCE (" << strokeSequence.size() << ")";
	for (Stroke stroke : strokeSequence)
	{
		if (stroke.scancode == SC_CPS_ESC)
		{
			if (stroke.isDownstroke)
			{
				if(inCpsEscape)
					error("Received double SC_CPS_ESC down.");
				else
				{
					if (globalState.modsTempAltered.size() > 0)
					{
						error("Internal error: previous escape sequence was not undone.");
						globalState.modsTempAltered.clear();
					}
				}
			}			
			else 
			{
				if (inCpsEscape) //play the escape sequence
				{
					for (Stroke strk : globalState.modsTempAltered)
					{
						sendStroke(strk);
						Sleep(delay);
					}
				}
				else //undo the previous sequence
				{
					IFDEBUG cout << endl << "play sequence::UNDO:" << globalState.modsTempAltered.size();
					for (Stroke strk : globalState.modsTempAltered)
					{
						strk.isDownstroke = !strk.isDownstroke;
						sendStroke(strk);
						Sleep(delay);
					}
					globalState.modsTempAltered.clear();
				}
			}
			inCpsEscape = stroke.isDownstroke;
			continue;
		}
		if (inCpsEscape)  //currently only one escape function: conditional break/make of modifiers
		{
			//the scancode is a modifier bitmask. Press/Release keys as necessary.
			//make modifier IF the system knows it is up
			unsigned short tempModChange = stroke.scancode;		//escaped scancode carries the modifier bitmask
			Stroke newstroke;

			if (stroke.isDownstroke)  //make modifiers when they are up
			{
				unsigned short exor = state.modifiers ^ tempModChange; //figure out which ones we must press
				tempModChange &= exor;
			}
			else	//break mods if down
				tempModChange &= state.modifiers;

			newstroke.isDownstroke = stroke.isDownstroke;

			const int NUMBER_OF_MODIFIERS_ALTERED_IN_SEQUENCES = 8; //high modifiers are skipped because they are never sent anyways.
			for (int i = 0; i < NUMBER_OF_MODIFIERS_ALTERED_IN_SEQUENCES; i++)  //push keycodes for all mods to be altered
			{
				unsigned short modBitmask = tempModChange & (1 << i);
				if (!modBitmask)
					continue;
				unsigned short sc = getModifierForBitmask(modBitmask);
				newstroke.scancode = sc;
				globalState.modsTempAltered.push_back(newstroke);
			}
			continue;
		}
		else //regular non-escaped stroke
		{
			sendStroke(stroke);
			if (stroke.scancode == AHK_HOTKEY1 || stroke.scancode == AHK_HOTKEY2)
				delay = DELAY_FOR_AHK;
			else
				Sleep(delay);
		}
	}
	if (inCpsEscape)
		error("SC_CPS escape sequence was not finished properly. Check your config.");
	if (globalState.oldmodsTempReleased.size() > 0)
		error("SC_CPS escape sequence temp released but did not re-make a modifier. Check your config.");
}

void playMacro(InterceptionKeyStroke macro[], int macroLength)
{
	unsigned int delay = mode.delayBetweenMacroKeysMS;
	IFDEBUG cout << "\t--> PLAY MACRO (" << macroLength << ")";
	Stroke stroke;
	for (int i = 0; i < macroLength; i++)
	{
		stroke = ikstroke2stroke(macro[i]);
		sendStroke(stroke);
		//IFDEBUG cout << " " << scLabels[macro[i].code] << getSymbolForStrokeState(macro[i].state);
		if (stroke.scancode == AHK_HOTKEY1 || stroke.scancode == AHK_HOTKEY2)
			delay = DELAY_FOR_AHK;
		Sleep(delay);  //local sys needs ~1ms, RDP fullscreen ~3 ms delay so they can keep up with macro key action
	}
}


void getHardwareId()
{
	{
		wchar_t  hardware_id[500] = { 0 };
		string id;
		size_t length = interception_get_hardware_id(globalState.interceptionContext, globalState.interceptionDevice, hardware_id, sizeof(hardware_id));
		if (length > 0 && length < sizeof(hardware_id))
		{
			wstring wid(hardware_id);
			string sid(wid.begin(), wid.end());
			id = sid;
		} 
		else
			id = "UNKNOWN_ID";

		globalState.deviceIdKeyboard = id;
		globalState.deviceIsAppleKeyboard = (id.find("VID_05AC") != string::npos);

		IFDEBUG cout << endl << "getHardwareId:" << id << " / Apple keyboard: " << globalState.deviceIsAppleKeyboard;
	}
}

void printHelloHeader()
{

	string line1 = "Capsicain v" + version;
#ifdef NDEBUG
	line1 += " (Release build)";
#else
	line1 += " (DEBUG build)";
#endif
	cout << endl;
	for (int i = 0; i < line1.length(); i++)
		cout << "-";
	cout << endl << line1 << endl;
	for (int i = 0; i < line1.length(); i++)
		cout << "-";
	cout << endl;
}

void printHelloHelp()
{
	cout << endl << endl;
	cout << "[ESC] + [X] to stop." << endl
		 << "[ESC] + [H] for Help";
	cout << endl << endl << "FEATURES"
		<< endl << (mode.lbSlashShift ? "ON :" : "OFF:") << "Backslash->Shift "
		<< endl << (mode.slashShift ? "ON :" : "OFF:") << "Slash->Shift "
		<< endl << (mode.flipZy ? "ON :" : "OFF:") << "Z<->Y "
		<< endl << (mode.flipAltWinOnAppleKeyboards ? "ON :" : "OFF:") << "Win<->Alt for Apple keyboards"
		;

}

void printHelp()
{
	cout << "HELP" << endl << endl
		<< "[ESC] + [{key}] for core commands" << endl
		<< "[X] Exit" << endl
		<< "[Q] (dev feature) Stop the debug build if both release and debug are running" << endl
		<< "[S] Status" << endl
		<< "[R] Reset" << endl
		<< "[D] Debug mode output" << endl
		<< "[E] Error log" << endl
		<< "[A] AHK start" << endl
		<< "[K] AHK end" << endl
		<< "[0]..[9] switch layers. [0] is the 'do nothing but listen for commands' layer" << endl
		<< "[Z] (labeled [Y] on GER keyboard): flip Y<>Z keys" << endl
		<< "[W] flip ALT <> WIN on Apple keyboards" << endl
		<< "[\\] (labeled [<] on GER keyboard): ISO boards only: key cut out of left shift -> Left Shift" << endl
		<< "[/] (labeled [-] on GER keyboard): Slash -> Right Shift" << endl
		<< "[C] switch character creation mode (Alt+Numpad or AHK)" << endl
		<< "[ and ]: pause between macro keys sent -/+ 10ms " << endl
		;
}
void printStatus()
{
	int numMakeReceived = 0;
	int numMakeSent = 0;
	for (int i = 0; i < 255; i++)
	{
		if (globalState.keysDownReceived[i])
			numMakeReceived++;
		if (globalState.keysDownSent[i])
			numMakeSent++;
	}
	cout << "STATUS" << endl << endl
		<< "Capsicain version: " << version << endl
		<< "hardware id:" << globalState.deviceIdKeyboard << endl
		<< "Apple keyboard: " << globalState.deviceIsAppleKeyboard << endl
		<< "active LAYER: " << mode.activeLayer << endl
		<< "modifier state: " << hex << state.modifiers << endl
		<< "delay between macro keys (ms): " << mode.delayBetweenMacroKeysMS << endl
		<< "character creation mode: " << mode.characterCreationMode << endl
		<< "# keys down received: " << numMakeReceived << endl
		<< "# keys down sent: " << numMakeSent << endl
		<< (errorLog.length() > 1 ? "ERROR LOG contains entries" : "clean error log") << " (" << errorLog.length() << " chars)" << endl
		;
}

void normalizeKeyStroke(InterceptionKeyStroke &stroke) {
	if (stroke.code > 0x7F) {
		stroke.code &= 0x7F;
		stroke.state |= 2;
	}
}

InterceptionKeyStroke stroke2ikstroke(Stroke stroke)
{
	InterceptionKeyStroke iks = { stroke.scancode, 0 };
	if (stroke.scancode >= 0x80)
	{
		iks.code = static_cast<unsigned short>(stroke.scancode & 0x7F);
		iks.state |= 2;
	}
	if (!stroke.isDownstroke)
		iks.state |= 1;

	return iks;
}

Stroke ikstroke2stroke(InterceptionKeyStroke ikStroke)
{	
	Stroke strk;
	strk.scancode = ikStroke.code;
	if ((ikStroke.state & 2) == 2)
		strk.scancode |= 0x80;
	strk.isDownstroke = ikStroke.state & 1 ? false : true;
	return strk;
}

void sendStroke(Stroke stroke)
{
	if (stroke.scancode == 0xE4)
		IFDEBUG cout << " (sending E4) ";
	if (stroke.scancode > 0xFF)
	{
		error("Unexpected scancode > 255: " + to_string(stroke.scancode));
		return;
	}
	if (!stroke.isDownstroke &&  !globalState.keysDownSent[(unsigned char)stroke.scancode])  //ignore up when key is already up
	{
		IFDEBUG cout << " >(blocked " << scLabels[stroke.scancode] << " UP: was not down)>";
		return;
	}
	globalState.keysDownSent[(unsigned char)stroke.scancode] = stroke.isDownstroke;

	InterceptionKeyStroke iks = stroke2ikstroke(stroke);
	IFDEBUG cout << " >" << scLabels[stroke.scancode] << (stroke.isDownstroke ? "v" : "^") << ">";

	interception_send(globalState.interceptionContext, globalState.interceptionDevice, (InterceptionStroke *)&iks, 1);
}

void reset()
{
	initAllStatesToDefault();
	resetCapsNumScrollLock();

	for (int i = 0; i < 255; i++)	//Send() suppresses key UP if it thinks it is already up.
		globalState.keysDownSent[i] = true;

	IFDEBUG cout << endl << "Resetting all modifiers to UP" << endl;
	vector<Stroke> strokeSequence;

	strokeSequence.push_back({SC_LSHIFT, false });
	strokeSequence.push_back({SC_RSHIFT, false });
	strokeSequence.push_back({SC_LCTRL, false});
	strokeSequence.push_back({SC_RCTRL, false});
	strokeSequence.push_back({SC_LWIN, false});
	strokeSequence.push_back({SC_RWIN, false});
	strokeSequence.push_back({SC_LALT, false});
	strokeSequence.push_back({SC_RALT, false});
	strokeSequence.push_back({SC_CAPS, false});
	strokeSequence.push_back({AHK_HOTKEY1, false});
	strokeSequence.push_back({AHK_HOTKEY2, false});
	playStrokeSequence(strokeSequence);

	for (int i = 0; i < 255; i++)
	{
		globalState.keysDownReceived[i] = false;
		globalState.keysDownSent[i] = false;
	}

	readIni();
}


void macroMakeKey(unsigned short scancode)
{
	state.keyMacro[state.keyMacroLength].code = scancode;
	state.keyMacro[state.keyMacroLength].state = KEYSTATE_DOWN;
	state.keyMacroLength++;
}
void macroBreakKey(unsigned short scancode)
{
	state.keyMacro[state.keyMacroLength].code = scancode;
	state.keyMacro[state.keyMacroLength].state = KEYSTATE_UP;
	state.keyMacroLength++;
}
void macroMakeBreakKey(unsigned short scancode)
{
	state.keyMacro[state.keyMacroLength].code = scancode;
	state.keyMacro[state.keyMacroLength].state = KEYSTATE_DOWN;
	state.keyMacroLength++;
	state.keyMacro[state.keyMacroLength].code = scancode;
	state.keyMacro[state.keyMacroLength].state = KEYSTATE_UP;
	state.keyMacroLength++;
}


//Macro wrapped in lshift off temporarily. Example: [shift+backspace] -> ctrl+Y  must suppress the shift, because shift+ctrl+Y is garbage
void createMacroKeyComboRemoveShift(int a, int b, int c, int d)
{
	if (IS_LSHIFT_DOWN)
		macroBreakKey(SC_LSHIFT);
	createMacroKeyCombo(a, b, c, d);
	if (IS_LSHIFT_DOWN)
		macroMakeKey(SC_LSHIFT);
}

//press all scancodes, then release them. Pass a,b,0,0 if you need less than 4
void createMacroKeyCombo(int a, int b, int c, int d)
{
	createMacroKeyComboNtimes(a, b, c, d, 1);
}
void createMacroKeyCombo10timesIfAltDown(int a, int b, int c, int d, unsigned short modifiers)
{
	if (modifiers & BITMASK_LALT)
		createMacroKeyComboNtimes(a, b, c, d, 10);
	else
		createMacroKeyComboNtimes(a, b, c, d, 1);
}
void createMacroKeyComboNtimes(int a, int b, int c, int d, int repeat)
{
	unsigned short scancodes[] = { (unsigned short)a, (unsigned short)b, (unsigned short)c, (unsigned short)d };
	for (int rep = 0; rep < repeat; rep++)
	{
		for (int i = 0; i < 4; i++)
		{
			if (scancodes[i] == 0)
				break;
			if (scancodes[i] == SC_LCTRL && IS_LCTRL_DOWN)
				continue;
			if (scancodes[i] == SC_LSHIFT && IS_LSHIFT_DOWN)
				continue;
			macroMakeKey(scancodes[i]);
		}

		for (int i = 3; i >= 0; i--)
		{
			if (scancodes[i] == 0)
				continue;
			if (scancodes[i] == SC_LCTRL && IS_LCTRL_DOWN)
				continue;
			if (scancodes[i] == SC_LSHIFT && IS_LSHIFT_DOWN)
				continue;
			macroBreakKey(scancodes[i]);
		}
	}
}

//virtually push Alt+Numpad 0 1 2 4  for special characters
//pass a b c 0 for 3-digit combos
void createMacroAltNumpad(unsigned short a, unsigned short b, unsigned short c, unsigned short d)
{
	unsigned short fsc[] = { a, b, c, d };

	bool lshift = (state.modifiers & BITMASK_LSHIFT) > 0;
	if (lshift)
		macroBreakKey(SC_LSHIFT);
	bool rshift = (state.modifiers & BITMASK_RSHIFT) > 0;
	if (rshift)
		macroBreakKey(SC_RSHIFT);
	macroMakeKey(SC_LALT);

	for (int i = 0; i < 4 && fsc[i] != 0; i++)
		macroMakeBreakKey(fsc[i]);

	macroBreakKey(SC_LALT);
	if (rshift)
		macroMakeKey(SC_RSHIFT);
	if (lshift)
		macroMakeKey(SC_LSHIFT);
}


/* Sending special character with scancodes is no fun.
	There are 3 createCharactermodes:
	1. IBM classic: ALT + NUMPAD 3-digits. Supported by some apps.
	2. ANSI: ALT + NUMPAD 4-digits. Supported by other apps, many Microsoft apps but not all.
	3. AHK: Sends F14|F15 + 1 base character. Requires an AHK script that translates these comobos to characters.
	This is Windows only. For Linux, the combo is [Ctrl]+[Shift]+[U] , {unicode E4 is �} , [Enter]
	For 1. and 2. NumLock must be ON
*/
void processCapsTapped(unsigned short scancd, int charCrtMode)
{
	bool shiftXorCaps = IS_SHIFT_DOWN != ((GetKeyState(VK_CAPITAL) & 0x0001) != 0);

	if (charCrtMode == 0) //IBM
	{
		switch (scancd)
		{
		case SC_O:
			if (shiftXorCaps)
				createMacroAltNumpad(SC_NP1, SC_NP5, SC_NP3, 0);
			else
				createMacroAltNumpad(SC_NP1, SC_NP4, SC_NP8, 0);
			break;
		case SC_A:
			if (shiftXorCaps)
				createMacroAltNumpad(SC_NP1, SC_NP4, SC_NP2, 0);
			else
				createMacroAltNumpad(SC_NP1, SC_NP3, SC_NP2, 0);
			break;
		case SC_U:
			if (shiftXorCaps)
				createMacroAltNumpad(SC_NP1, SC_NP5, SC_NP4, 0);
			else
				createMacroAltNumpad(SC_NP1, SC_NP2, SC_NP9, 0);
			break;
		case SC_S:
			createMacroAltNumpad(SC_NP2, SC_NP2, SC_NP5, 0);
			break;
		case SC_E:
			createMacroAltNumpad(SC_NP0, SC_NP1, SC_NP2, SC_NP8);
			break;
		case SC_D:
			createMacroAltNumpad(SC_NP1, SC_NP6, SC_NP7, 0);
			break;
		case SC_T: // test print from [1] to [Enter]
		{
			for (unsigned short i = 2; i <= 0x1C; i++)
				macroMakeBreakKey(i);
			break;
		}
		case SC_R: // test2: print 50x10 �
		{
			for (int outer = 0; outer < 4; outer++)
			{
				for (unsigned short i = 0; i < 40; i++)
				{

					macroMakeKey(SC_LALT);
					macroMakeBreakKey(SC_NP1);
					macroMakeBreakKey(SC_NP3);
					macroMakeBreakKey(SC_NP2);
					macroBreakKey(SC_LALT);
				}

				macroMakeBreakKey(SC_RETURN);
			}
			break;
		}
		case SC_L: // Linux test: print 50x10 �
		{
			for (int outer = 0; outer < 4; outer++)
			{
				for (unsigned short i = 0; i < 40; i++)
				{

					macroMakeKey(SC_LCTRL);
					macroMakeKey(SC_LSHIFT);
					macroMakeKey(SC_U);
					macroBreakKey(SC_U);
					macroBreakKey(SC_LSHIFT);
					macroBreakKey(SC_LCTRL);
					macroMakeBreakKey(SC_E);
					macroMakeBreakKey(SC_4);
					macroMakeBreakKey(SC_RETURN);
				}
				macroMakeBreakKey(SC_RETURN);
			}
			break;
		}
		}
	}
	else if (charCrtMode == 1) //ANSI
	{
		switch (scancd)
		{
		case SC_O:
			if (shiftXorCaps)
				createMacroAltNumpad(SC_NP0, SC_NP2, SC_NP1, SC_NP4);
			else
				createMacroAltNumpad(SC_NP0, SC_NP2, SC_NP4, SC_NP6);
			break;
		case SC_A:
			if (shiftXorCaps)
				createMacroAltNumpad(SC_NP0, SC_NP1, SC_NP9, SC_NP6);
			else
				createMacroAltNumpad(SC_NP0, SC_NP2, SC_NP2, SC_NP8);
			break;
		case SC_U:
			if (shiftXorCaps)
				createMacroAltNumpad(SC_NP0, SC_NP2, SC_NP2, SC_NP0);
			else
				createMacroAltNumpad(SC_NP0, SC_NP2, SC_NP5, SC_NP2);
			break;
		case SC_S:
			createMacroAltNumpad(SC_NP0, SC_NP2, SC_NP2, SC_NP3);
			break;
		case SC_E:
			createMacroAltNumpad(SC_NP0, SC_NP1, SC_NP2, SC_NP8);
			break;
		case SC_D:
			createMacroAltNumpad(SC_NP1, SC_NP7, SC_NP6, 0);
			break;
		}
	}
	else if (charCrtMode == 2) //AHK
	{
		switch (scancd)
		{
		case SC_A:
		case SC_O:
		case SC_U:
			createMacroKeyCombo(shiftXorCaps ? AHK_HOTKEY2 : AHK_HOTKEY1, scancd, 0, 0);
			break;
		case SC_S:
		case SC_E:
		case SC_D:
		case SC_C:
			createMacroKeyCombo(AHK_HOTKEY1, scancd, 0, 0);
			break;
		}
	}

	if (state.keyMacroLength == 0)
	{
		switch (scancd) {
		case SC_0:
		case SC_1:
		case SC_2:
		case SC_3:
		case SC_4:
		case SC_5:
		case SC_6:
		case SC_7:
		case SC_8:
		case SC_9:
			createMacroKeyCombo(AHK_HOTKEY2, scancd, 0, 0);
			break;
		}
	}
}

string getSymbolForStrokeState(unsigned short state)
{
	switch (state)
	{
	case 0: return "v";
	case 1: return "^";
	case 2: return "*v";
	case 3: return "*^";
	}
	return "???" + state;
}

