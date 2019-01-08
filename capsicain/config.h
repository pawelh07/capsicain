#pragma once
#include <string>
#include <vector>

struct KeyEvent
{
    unsigned short scancode = 0;
    bool isDownstroke = true;
};

bool readIniFile(std::vector<std::string>& iniLines);

std::vector<std::string> getSection_nOrDefaultFromIni(std::string sectionName, int sectionVersion, std::vector<std::string> iniContent);
std::vector<std::string> getSectionFromIni(std::string sectionName, std::vector<std::string> iniContent);
bool sectionHasKey(std::string key, std::vector<std::string> iniLines);
bool getStringValueForKeyInSection(std::string key, std::string & value, std::vector<std::string> sectionLines);
bool getIntValueForKeyInSection(std::string key, int & value, std::vector<std::string> sectionLines);
bool parseCombo(std::string &funcParams, std::string * scLabels, std::vector<KeyEvent> &strokeSeq, bool &retflag);
bool parseModCombo(std::string line, unsigned short &key, unsigned short(&mods)[3], std::vector<KeyEvent> &strokeSequence, std::string scLabels[]);
bool parseTwoTokenMapping(std::string line, unsigned char & keyIn, unsigned char & keyOut, std::string scLabels[]);
bool parseMapFromTo(std::string mapFromTo, unsigned char(&alphamap)[256], std::string scLabels[]);
bool parseThreeTokenMapping(std::string line, unsigned char & keyA, unsigned char & keyB, unsigned char & keyC, std::string scLabels[]);
