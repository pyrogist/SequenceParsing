/*
 SequenceParser is a small class that helps reading a sequence of images within a directory.

 
 Copyright (C) 2013 INRIA
 Author Alexandre Gauthier-Foichat alexandre.gauthier-foichat@inria.fr
 
 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:
 
 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.
 
 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
 INRIA
 Domaine de Voluceau
 Rocquencourt - B.P. 105
 78153 Le Chesnay Cedex - France
 
 */
#include "SequenceParsing.h"

#include <cassert>
#include <cmath>
#include <climits>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <locale>
#include <istream>
#include <algorithm>


#include "tinydir/tinydir.h"


// Use: #pragma message WARN("My message")
#if _MSC_VER
#   define FILE_LINE_LINK __FILE__ "(" STRINGISE(__LINE__) ") : "
#   define WARN(exp) (FILE_LINE_LINK "WARNING: " exp)
#else//__GNUC__ - may need other defines for different compilers
#   define WARN(exp) ("WARNING: " exp)
#endif

///the maximum number of non existing frame before Natron gives up trying to figure out a sequence layout.
#define NATRON_DIALOG_MAX_SEQUENCES_HOLE 1000

namespace  {


/**
     * @brief Given the pattern unpathed without extension (e.g: "filename###") and the file extension (e.g "jpg") ; this
     * functions extracts the common parts and the variables of the pattern ordered from left to right .
     * For example: file%04dname### and the jpg extension would return:
     * 3 common parts: "file","name",".jpg"
     * 2 variables: "%04d", "###"
     * The variables by order vector's second member is an int indicating how many non-variable (chars belonging to common parts) characters
     * were found before this variable.
     **/
static bool extractCommonPartsAndVariablesFromPattern(const std::string& patternUnPathedWithoutExt,
                                                      const std::string& patternExtension,
                                                      StringList* commonParts,
                                                      std::vector<std::pair<std::string,int> >* variablesByOrder) {
    int i = 0;
    bool inPrintfLikeArg = false;
    int printfLikeArgIndex = 0;
    std::string commonPart;
    std::string variable;
    int commonCharactersFound = 0;
    bool previousCharIsSharp = false;
    while (i < (int)patternUnPathedWithoutExt.size()) {
        const char& c = patternUnPathedWithoutExt.at(i);
        if (c == '#') {
            if (!commonPart.empty()) {
                commonParts->push_back(commonPart);
                commonCharactersFound += commonPart.size();
                commonPart.clear();
            }
            if (!previousCharIsSharp && !variable.empty()) {
                variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                variable.clear();
            }
            variable.push_back(c);
            previousCharIsSharp = true;
        } else if (c == '%') {

            char next = '\0';
            if (i < (int)patternUnPathedWithoutExt.size() - 1) {
                next = patternUnPathedWithoutExt.at(i + 1);
            }
            char prev = '\0';
            if (i > 0) {
                prev = patternUnPathedWithoutExt.at(i -1);
            }

            if (next == '\0') {
                ///if we're at end, just consider the % character as any other
                commonPart.push_back(c);
            } else if (prev == '%') {
                ///we escaped the previous %, append this one to the text
                commonPart.push_back(c);
            } else if (next != '%') {
                ///if next == % then we have escaped the character
                ///we don't support nested  variables
                if (inPrintfLikeArg) {
                    return false;
                }
                printfLikeArgIndex = 0;
                inPrintfLikeArg = true;
                if (!commonPart.empty()) {
                    commonParts->push_back(commonPart);
                    commonCharactersFound += commonPart.size();
                    commonPart.clear();
                }
                if (!variable.empty()) {
                    variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                    variable.clear();
                }
                variable.push_back(c);
            }
        } else if ((c == 'd' || c == 'v' || c == 'V')  && inPrintfLikeArg) {
            inPrintfLikeArg = false;
            assert(!variable.empty());
            variable.push_back(c);
            variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
            variable.clear();
        } else if (inPrintfLikeArg) {
            ++printfLikeArgIndex;
            assert(!variable.empty());
            variable.push_back(c);
            ///if we're after a % character, and c is a letter different than d or v or V
            ///or c is digit different than 0, then we don't support this printf like style.
            if (std::isalpha(c) ||
                    (printfLikeArgIndex == 1 && c != '0')) {
                commonParts->push_back(variable);
                commonCharactersFound += variable.size();
                variable.clear();
                inPrintfLikeArg = false;
            }

        } else {
            commonPart.push_back(c);
            if (!variable.empty()) {
                variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                variable.clear();
            }
        }
        ++i;
    }

    if (!commonPart.empty()) {
        commonParts->push_back(commonPart);
        commonCharactersFound += commonPart.size();
    }
    if (!variable.empty()) {
        variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
    }

    if (!patternExtension.empty()) {
        commonParts->push_back(std::string('.' + patternExtension));
    }
    return true;
}


// templated version of my_equal so it could work with both char and wchar_t
template<typename charT>
struct my_equal {
    my_equal( const std::locale& loc ) : loc_(loc) {}
    bool operator()(charT ch1, charT ch2) {
        return std::toupper(ch1, loc_) == std::toupper(ch2, loc_);
    }
private:
    const std::locale& loc_;
};

// find substring (case insensitive)
template<typename T>
size_t ci_find_substr( const T& str1, const T& str2, const std::locale& loc = std::locale() )
{
    typename T::const_iterator it = std::search( str1.begin(), str1.end(),
                                                 str2.begin(), str2.end(), my_equal<typename T::value_type>(loc) );
    if ( it != str1.end() ) return it - str1.begin();
    else return std::string::npos; // not found
}

static size_t findStr(const std::string& from,const std::string& toSearch,int pos, bool caseSensitive = false)
{
    if (caseSensitive) {
        return from.find(toSearch,pos);
    } else {
        return ci_find_substr<std::string>(from, toSearch);
    }
}


static bool startsWith(const std::string& str,const std::string& prefix,bool caseSensitive = false)
{
    return findStr(str,prefix,0,caseSensitive) == 0;
}

static bool endsWith(const std::string &str, const std::string &suffix)
{
    return str.size() >= suffix.size() &&
            str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static void removeAllOccurences(std::string& str,const std::string& toRemove,bool caseSensitive = false)
{
    if (str.size()) {
        size_t i = 0;
        while ((i = findStr(str, toRemove, i,caseSensitive)) != std::string::npos) {
            str.erase(i,toRemove.size());
        }
    }
}

static int stringToInt(const std::string& str)
{
    std::stringstream ss(str);
    int ret = 0;
    try {
        ss >> ret;
    } catch (const std::ios_base::failure& e) {
        return 0;
    }
    return ret;
}

static std::string stringFromInt(int nb)
{
    std::stringstream ss;
    ss << nb;
    return ss.str();
}

static std::string removeFileExtension(std::string& filename) {
    int i = filename.size() -1;
    std::string extension;
    while(i>=0 && filename.at(i) != '.') {
        extension.insert(0,1,filename.at(i));
        --i;
    }
    filename = filename.substr(0,i);
    return extension;
}

static void getFilesFromDir(tinydir_dir& dir,StringList* ret)
{
    ///iterate through all the files in the directory
    while (dir.has_next) {
        tinydir_file file;
        tinydir_readfile(&dir, &file);

        if (file.is_dir) {
            tinydir_next(&dir);
            continue;
        }

        std::string filename(file.name);
        if (filename == "." || filename == "..") {
            tinydir_next(&dir);
            continue;
        } else {
            ret->push_back(filename);
        }

        tinydir_next(&dir);

    }
}

/**
     * @brief Given the variable token (e.g: %04d or #### or %v or %V, check if the variable and its type
     * coherent with the symbol. If so this function returns true, otherwise false.
     * @param frameNumberOrViewNumber [out] The frame number extracted from the variable, or the view number.
     * The view index is always 0 if there's only a single view.
     * If there'are 2 views, index 0 is considered to be the left view and index 1 is considered to be the right view.
     * If there'are multiple views, the index is corresponding to the view
     * Type is an integer indicating how is the content of the string supposed to be interpreted:
     * 0: a frame number (e.g: 0001)
     * 1: a short view number (e.g: 'l' or 'r')
     * 2: a long view number (e.g: 'left' or 'LEFT' or 'right' or 'RIGHT' or  'view0' or 'VIEW0')
     **/
static bool checkVariable(const std::string& variableToken,const std::string& variable,int type,int* frameNumberOrViewNumber) {
    if (variableToken == "%v") {
        if (type != 1) {
            return false;
        }
        if (variable == "r") {
            *frameNumberOrViewNumber = 1;
            return true;
        } else if (variable == "l") {
            *frameNumberOrViewNumber = 0;
            return true;
        } else if (startsWith(variable,"view")) {
            std::string viewNo = variable;
            removeAllOccurences(viewNo,"view");
            *frameNumberOrViewNumber = stringToInt(viewNo);
            return true;
        } else {
            return false;
        }

    } else if (variableToken == "%V") {
        if (type != 2) {
            return false;
        }
        if (variable == "right") {
            *frameNumberOrViewNumber = 1;
            return true;
        } else if (variable == "left") {
            *frameNumberOrViewNumber = 0;
            return true;
        } else if (startsWith(variable,"view")) {
            std::string viewNo = variable;
            removeAllOccurences(viewNo, "view");
            *frameNumberOrViewNumber = stringToInt(viewNo);
            return true;
        } else {
            return false;
        }
    } else if (variableToken.find('#') != std::string::npos) {
        if (type != 0) {
            return false;
        } else if(variable.size() < variableToken.size()) {
            return false;
        }
        int prepending0s = 0;
        int i = 0;
        while (i < (int)variable.size()) {
            if (variable.at(i) != '0') {
                break;
            } else {
                ++prepending0s;
            }
            ++i;
        }

        ///extra padding on numbers bigger than the hash chars count are not allowed.
        if (variable.size() > variableToken.size() && prepending0s > 0) {
            return false;
        }

        *frameNumberOrViewNumber = stringToInt(variable);
        return true;
    } else if (startsWith(variableToken,"%0") && endsWith(variableToken,"d")) {
        if (type != 0) {
            return false;
        }
        std::string extraPaddingCountStr = variableToken;
        removeAllOccurences(extraPaddingCountStr, "%0");
        removeAllOccurences(extraPaddingCountStr, "d");
        int extraPaddingCount = stringToInt(extraPaddingCountStr);
        if ((int)variable.size() < extraPaddingCount) {
            return false;
        }

        int prepending0s = 0;
        int i = 0;
        while (i < (int)variable.size()) {
            if (variable.at(i) != '0') {
                break;
            } else {
                ++prepending0s;
            }
            ++i;
        }

        ///extra padding on numbers bigger than the hash chars count are not allowed.
        if ((int)variable.size() > extraPaddingCount && prepending0s > 0) {
            return false;
        }

        *frameNumberOrViewNumber = stringToInt(variable);
        return true;
    } else if (variableToken == "%d") {
        *frameNumberOrViewNumber = stringToInt(variable);
        return true;
    } else {
        throw std::invalid_argument("Variable token unrecognized: " + variableToken);
    }
}

/**
     * @brief Tries to match a given filename with the common parts and the variables of a pattern.
     * Note that if 2 variables have the exact same meaning (e.g: ### and %04d) and they do not correspond to the
     * same frame number it will reject the filename against the pattern.
     * @see extractCommonPartsAndVariablesFromPattern
     **/
static bool matchesPattern(const std::string& filename,const StringList& commonPartsOrdered,
                           const std::vector<std::pair<std::string,int> >& variablesOrdered,
                           int* frameNumber,int* viewNumber) {

    ///initialize the view number
    *viewNumber = -1;

    int lastPartPos = -1;
    for (size_t i = 0; i < commonPartsOrdered.size(); ++i) {
#pragma message WARN("This line will match common parts that could be longer,e.g: marleen would match marleenBG ")
        size_t pos = findStr(filename, commonPartsOrdered[i], lastPartPos == -1? 0 : lastPartPos,true);
        ///couldn't find a common part
        if (pos == std::string::npos) {
            return false;
        }

        //the common parts order is not the same.
        if ((int)pos <= lastPartPos) {
            return false;
        }
    }


    if (variablesOrdered.empty()) {
        return true;
    }

    ///if we reach here that means the filename contains all the common parts. We can now expand the variables and start
    ///looking for a matching pattern

    ///we first extract all interesting informations from the filename by order
    ///i.e: we extract the digits and the view names.

    ///for each variable, a string representing the number (e.g: 0001 or LEFT/RIGHT or even VIEW0)
    ///corresponding either to a view number or a frame number


    int i = 0;
    std::string variable;
    ///extracting digits
    int commonCharactersFound = 0;
    bool previousCharIsDigit = false;
    bool wasFrameNumberSet = false;
    bool wasViewNumberSet = false;
    int variableChecked = 0;

    ///the index in variablesOrdered to check
    int nextVariableIndex = 0;

    while (i < (int)filename.size()) {
        const char& c = filename.at(i);
        if (std::isdigit(c)) {

            assert((!previousCharIsDigit && variable.empty()) || previousCharIsDigit);
            previousCharIsDigit = true;
            variable.push_back(c);
            ++i;
        } else {
            previousCharIsDigit = false;

            if (!variable.empty()) {
                int fnumber;

                ///the pattern wasn't expecting a frame number or the digits count doesn't respect the
                ///hashes character count or the printf-like padding count is wrong.
                if (variablesOrdered[nextVariableIndex].second != commonCharactersFound) {
                    return false;
                }
                if (!checkVariable(variablesOrdered[nextVariableIndex].first, variable, 0, &fnumber)) {
                    return false;
                }

                ///a previous frame number variable had a different frame number
                if (wasFrameNumberSet && fnumber != *frameNumber) {
                    return false;
                }
                ++variableChecked;
                ++nextVariableIndex;
                wasFrameNumberSet = true;
                *frameNumber = fnumber;
                variable.clear();
            }

            char clower = std::tolower(c);
            ///these are characters that trigger a view name, start looking for a view name
            if(clower == 'l' || clower == 'r'  || clower == 'v') {
                std::string mid = filename.substr(i);
                bool startsWithLeft = startsWith(mid,"left");
                bool startsWithRight = startsWith(mid,"right");
                bool startsWithView = startsWith(mid,"view");

                if (startsWith(mid,"l") && !startsWithLeft) {

                    ///don't be so harsh with just short views name because the letter
                    /// 'l' or 'r' might be found somewhere else in the filename, so if theres
                    ///no variable expected here just continue

                    if (variablesOrdered[nextVariableIndex].first == std::string("%v") &&
                            variablesOrdered[nextVariableIndex].second == commonCharactersFound) {
                        ///the view number doesn't correspond to a previous view variable
                        if (wasViewNumberSet && *viewNumber != 0) {
                            return false;
                        }
                        wasViewNumberSet = true;
                        *viewNumber = 0;
                        ++variableChecked;
                        ++nextVariableIndex;
                    } else {
                        ++commonCharactersFound;
                    }
                    ++i;
                } else if (startsWith(mid,"r") && !startsWithRight) {
                    ///don't be so harsh with just short views name because the letter
                    /// 'l' or 'r' might be found somewhere else in the filename, so if theres
                    ///no variable expected here just continue
                    if (variablesOrdered[nextVariableIndex].first == "%v"  &&
                            variablesOrdered[nextVariableIndex].second == commonCharactersFound) {
                        ///the view number doesn't correspond to a previous view variable
                        if (wasViewNumberSet &&  *viewNumber != 1) {
                            return false;
                        }
                        wasViewNumberSet = true;
                        *viewNumber = 1;
                        ++variableChecked;
                        ++nextVariableIndex;
                    } else {
                        ++commonCharactersFound;
                    }
                    ++i;
                } else if (startsWithLeft) {

                    int viewNo;

                    ///the pattern didn't expect a view name here
                    if (variablesOrdered[nextVariableIndex].second != commonCharactersFound) {
                        return false;
                    }
                    if (!checkVariable(variablesOrdered[nextVariableIndex].first, "left", 2, &viewNo)) {
                        return false;
                    }
                    ///the view number doesn't correspond to a previous view variable
                    if (wasViewNumberSet && viewNo != *viewNumber) {
                        return false;
                    }
                    ++nextVariableIndex;
                    ++variableChecked;
                    wasViewNumberSet = true;
                    *viewNumber = viewNo;

                    i += 4;
                } else if (startsWithRight) {

                    int viewNo;

                    ///the pattern didn't expect a view name here
                    if (variablesOrdered[nextVariableIndex].second != commonCharactersFound) {
                        return false;
                    }
                    if (!checkVariable(variablesOrdered[nextVariableIndex].first, "right", 2, &viewNo)) {
                        return false;
                    }
                    ///the view number doesn't correspond to a previous view variable
                    if (wasViewNumberSet && viewNo != *viewNumber) {
                        return false;
                    }
                    ++variableChecked;
                    ++nextVariableIndex;
                    wasViewNumberSet = true;
                    *viewNumber = viewNo;
                    i += 5;
                } else if(startsWithView) {
                    ///extract the view number
                    variable += "view";
                    std::string viewNumberStr;
                    int j = 4;
                    while (j < (int)mid.size() && std::isdigit(mid.at(j))) {
                        viewNumberStr.push_back(mid.at(j));
                        ++j;
                    }
                    if (!viewNumberStr.empty()) {
                        variable += viewNumberStr;

                        int viewNo;

                        if (variablesOrdered[nextVariableIndex].second != commonCharactersFound) {
                            return false;
                        }
                        const std::string& variableToken = variablesOrdered[nextVariableIndex].first;
                        ///if the variableToken is %v just put a type of 1
                        ///otherwise a type of 2, this is because for either %v or %V we write view<N>
                        int type = variableToken == "%v" ? 1 : 2;
                        if (!checkVariable(variableToken, variable, type, &viewNo)) {
                            ///the pattern didn't expect a view name here
                            return false;
                        }
                        ///the view number doesn't correspond to a previous view variable
                        if (wasViewNumberSet && viewNo != *viewNumber) {
                            return false;
                        }
                        ++variableChecked;
                        ++nextVariableIndex;
                        wasViewNumberSet = true;
                        *viewNumber = viewNo;

                        i += variable.size();
                    } else {
                        commonCharactersFound += 4;
                        i += 4;
                    }
                } else {
                    ++commonCharactersFound;
                    ++i;
                }
                variable.clear();

            } else {
                ++commonCharactersFound;
                ++i;
            }
        }
    }
    if (variableChecked == (int)variablesOrdered.size()) {
        return true;
    } else {
        return false;
    }
}


}


namespace SequenceParsing {
/**
     * @brief A small structure representing an element of a file name.
     * It can be either a text part, or a view part or a frame number part.
     **/
struct FileNameElement {

    enum Type { TEXT = 0  , FRAME_NUMBER };

    FileNameElement(const std::string& data,FileNameElement::Type type)
        : data(data)
        , type(type)
    {}

    std::string data;
    Type type;
};


////////////////////FileNameContent//////////////////////////

struct FileNameContentPrivate {
    ///Ordered from left to right, these are the elements composing the filename without its path
    std::vector<FileNameElement> orderedElements;
    std::string absoluteFileName;
    std::string filePath; //< the filepath
    std::string filename; //< the filename without path
    std::string extension; //< the file extension
    bool hasSingleNumber;
    std::string generatedPattern;

    FileNameContentPrivate()
        : orderedElements()
        , absoluteFileName()
        , filePath()
        , filename()
        , extension()
        , hasSingleNumber(false)
        , generatedPattern()
    {
    }

    void parse(const std::string& absoluteFileName);
};


FileNameContent::FileNameContent(const std::string& absoluteFilename)
    : _imp(new FileNameContentPrivate())
{
    _imp->parse(absoluteFilename);
}

FileNameContent::FileNameContent(const FileNameContent& other)
    : _imp(new FileNameContentPrivate())
{
    *this = other;
}

FileNameContent::~FileNameContent() {
    delete _imp;
}

void FileNameContent::operator=(const FileNameContent& other) {
    _imp->orderedElements = other._imp->orderedElements;
    _imp->absoluteFileName = other._imp->absoluteFileName;
    _imp->filename = other._imp->filename;
    _imp->filePath = other._imp->filePath;
    _imp->extension = other._imp->extension;
    _imp->hasSingleNumber = other._imp->hasSingleNumber;
    _imp->generatedPattern = other._imp->generatedPattern;
}

void FileNameContentPrivate::parse(const std::string& absoluteFileName) {
    this->absoluteFileName = absoluteFileName;
    filename = absoluteFileName;
    filePath = removePath(filename);

    int i = 0;
    std::string lastNumberStr;
    std::string lastTextPart;
    while (i < (int)filename.size()) {
        const char& c = filename.at(i);
        if (std::isdigit(c)) {
            lastNumberStr += c;
            if (!lastTextPart.empty()) {
                orderedElements.push_back(FileNameElement(lastTextPart,FileNameElement::TEXT));
                lastTextPart.clear();
            }
        } else {
            if (!lastNumberStr.empty()) {
                orderedElements.push_back(FileNameElement(lastNumberStr,FileNameElement::FRAME_NUMBER));
                if (!hasSingleNumber) {
                    hasSingleNumber = true;
                } else {
                    hasSingleNumber = false;
                }
                lastNumberStr.clear();
            }

            lastTextPart.push_back(c);
        }
        ++i;
    }

    if (!lastNumberStr.empty()) {
        orderedElements.push_back(FileNameElement(lastNumberStr,FileNameElement::FRAME_NUMBER));
        if (!hasSingleNumber) {
            hasSingleNumber = true;
        } else {
            hasSingleNumber = false;
        }
        lastNumberStr.clear();
    }
    if (!lastTextPart.empty()) {
        orderedElements.push_back(FileNameElement(lastTextPart,FileNameElement::TEXT));
        lastTextPart.clear();
    }



    size_t lastDotPos = filename.find_last_of('.');
    if (lastDotPos != std::string::npos) {
        int j = filename.size() - 1;
        while (j > 0 && filename.at(j) != '.') {
            extension.insert(0,1,filename.at(j));
            --j;
        }
    }

    ///now build the generated pattern with the ordered elements.
    int numberIndex = 0;
    for (unsigned int j = 0; j < orderedElements.size(); ++j) {
        const FileNameElement& e = orderedElements[j];
        switch (e.type) {
        case FileNameElement::TEXT:
            generatedPattern.append(e.data);
            break;
        case FileNameElement::FRAME_NUMBER:
        {
            std::string hashStr;
            int c = 0;
            while (c < (int)e.data.size()) {
                hashStr.push_back('#');
                ++c;
            }
            generatedPattern.append(hashStr + stringFromInt(numberIndex));
            ++numberIndex;
        } break;
        default:
            break;
        }
    }

}

StringList FileNameContent::getAllTextElements() const {
    StringList ret;
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type == FileNameElement::TEXT) {
            ret.push_back(_imp->orderedElements[i].data);
        }
    }
    return ret;
}

/**
     * @brief Returns the file path, e.g: /Users/Lala/Pictures/ with the trailing separator.
     **/
const std::string& FileNameContent::getPath() const {
    return _imp->filePath;
}

/**
     * @brief Returns the filename without its path.
     **/
const std::string& FileNameContent::fileName() const {
    return _imp->filename;
}

/**
     * @brief Returns the absolute filename as it was given in the constructor arguments.
     **/
const std::string& FileNameContent::absoluteFileName() const {
    return _imp->absoluteFileName;
}

const std::string& FileNameContent::getExtension() const {
    return _imp->extension;
}


/**
     * @brief Returns true if a single number was found in the filename.
     **/
bool FileNameContent::hasSingleNumber() const {
    return _imp->hasSingleNumber;
}

/**
     * @brief Returns true if the filename is composed only of digits.
     **/
bool FileNameContent::isFileNameComposedOnlyOfDigits() const {
    if ((_imp->orderedElements.size() == 1 || _imp->orderedElements.size() == 2)
            && _imp->orderedElements[0].type == FileNameElement::FRAME_NUMBER) {
        return true;
    } else {
        return false;
    }
}

/**
     * @brief Returns the file pattern found in the filename with hash characters style for frame number (i.e: ###)
     **/
const std::string& FileNameContent::getFilePattern() const {
    return _imp->generatedPattern;
}

/**
     * @brief If the filename is composed of several numbers (e.g: file08_001.png),
     * this functions returns the number at index as a string that will be stored in numberString.
     * If Index is greater than the number of numbers in the filename or if this filename doesn't
     * contain any number, this function returns false.
     **/
bool FileNameContent::getNumberByIndex(int index,std::string* numberString) const {

    int numbersElementsIndex = 0;
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
            if (numbersElementsIndex == index) {
                *numberString = _imp->orderedElements[i].data;
                return true;
            }
            ++numbersElementsIndex;
        }
    }
    return false;
}

/**
     * @brief Given the pattern of this file, it tries to match the other file name to this
     * pattern.
     * @param numberIndexToVary [out] In case the pattern contains several numbers (@see getNumberByIndex)
     * this value will be fed the appropriate number index that should be used for frame number.
     * For example, if this filename is myfile001_000.jpg and the other file is myfile001_001.jpg
     * numberIndexToVary would be 1 as the frame number string indentied in that case is the last number.
     * @returns True if it identified 'other' as belonging to the same sequence, false otherwise.
     **/
bool FileNameContent::matchesPattern(const FileNameContent& other,std::vector<int>* numberIndexesToVary) const {
    const std::vector<FileNameElement>& otherElements = other._imp->orderedElements;
    if (otherElements.size() != _imp->orderedElements.size()) {
        return false;
    }

    ///potential frame numbers are pairs of strings from this filename and the same
    ///string in the other filename.
    ///Same numbers are not inserted in this vector.
    std::vector< std::pair< int, std::pair<std::string,std::string> > > potentialFrameNumbers;
    int numbersCount = 0;
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type != otherElements[i].type) {
            return false;
        }
        if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
            if (_imp->orderedElements[i].data != otherElements[i].data) {
                ///if one frame number string is longer than the other, make sure it is because the represented number
                ///is bigger and not because there's extra padding
                /// For example 10000 couldve been produced with ## only and is valid, and 01 would also produce be ##.
                /// On the other hand 010000 could never have been produced with ## hence it is not valid.

                bool valid = true;
                ///if they have different sizes, if one of them starts with a 0 its over.
                if (_imp->orderedElements[i].data.size() != otherElements[i].data.size()) {

                    if (_imp->orderedElements[i].data.size() > otherElements[i].data.size()) {
                        if (otherElements[i].data.at(0) == '0' && otherElements[i].data.size() > 1) {
                            valid = false;
                        } else {
                            int k = 0;
                            int diff = std::abs((int)_imp->orderedElements[i].data.size()  - (int)otherElements[i].data.size());
                            while (k < (int)_imp->orderedElements[i].data.size() && k < diff) {
                                if (_imp->orderedElements[i].data.at(k) == '0') {
                                    valid = false;
                                }
                                break;
                                ++k;
                            }
                        }
                    } else {
                        if (_imp->orderedElements[i].data.at(0) == '0' && _imp->orderedElements[i].data.size() > 1) {
                            valid = false;
                        } else {
                            int k = 0;
                            int diff = std::abs((int)_imp->orderedElements[i].data.size()  - (int)otherElements[i].data.size());
                            while (k < (int)otherElements[i].data.size() && k < diff) {
                                if (otherElements[i].data.at(k) == '0') {
                                    valid = false;
                                }
                                break;
                                ++k;
                            }
                        }
                    }

                }
                if (valid) {
                    potentialFrameNumbers.push_back(std::make_pair(numbersCount,
                                                                   std::make_pair(_imp->orderedElements[i].data, otherElements[i].data)));
                }

            }
            ++numbersCount;
        } else if (_imp->orderedElements[i].type == FileNameElement::TEXT && _imp->orderedElements[i].data != otherElements[i].data) {
            return false;
        }
    }
    ///strings are identical
    if (potentialFrameNumbers.empty()) {
        return false;
    }

    ///find out in the potentialFrameNumbers what is the minimum with pairs and pick it up
    /// for example if 1 pair is : < 0001, 802398 > and the other pair is < 01 , 10 > we pick
    /// the second one.
    std::vector<int> minIndexes;
    int minimum = INT_MAX;
    for (unsigned int i = 0; i < potentialFrameNumbers.size(); ++i) {
        int thisNumber = stringToInt(potentialFrameNumbers[i].second.first);
        int otherNumber = stringToInt(potentialFrameNumbers[i].second.second);
        int diff = std::abs(thisNumber - otherNumber);
        if (diff < minimum) {
            minimum = diff;
            minIndexes.clear();
            minIndexes.push_back(i);
        } else if (diff == minimum) {
            minIndexes.push_back(i);
        }
    }
    for (unsigned int i = 0; i < minIndexes.size(); ++i) {
        numberIndexesToVary->push_back(potentialFrameNumbers[minIndexes[i]].first);
    }
    return true;

}

bool FileNameContent::generatePatternWithFrameNumberAtIndexes(const std::vector<int>& indexes,std::string* pattern) const {
    int numbersCount = 0;
    size_t lastNumberPos = 0;
    std::string indexedPattern = getFilePattern();
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
            lastNumberPos = findStr(indexedPattern, "#", lastNumberPos,true);
            assert(lastNumberPos != std::string::npos);

            int endTagPos = lastNumberPos;
            while (endTagPos < (int)indexedPattern.size() && indexedPattern.at(endTagPos) == '#') {
                ++endTagPos;
            }

            ///assert that the end of the tag is composed of  a digit
            if (endTagPos < (int)indexedPattern.size()) {
                assert(std::isdigit(indexedPattern.at(endTagPos)));
            }

            bool isNumberAFrameNumber = false;
            for (unsigned int j = 0; j < indexes.size(); ++j) {
                if (indexes[j] == numbersCount) {
                    isNumberAFrameNumber = true;
                    break;
                }
            }
            if (!isNumberAFrameNumber) {
                ///if this is not the number we're interested in to keep the ###, just expand the variable
                ///replace the whole tag with the original data
                indexedPattern.replace(lastNumberPos, endTagPos - lastNumberPos + 1, _imp->orderedElements[i].data);
            } else {
                ///remove the index of the tag and keep the tag.
                if (endTagPos < (int)indexedPattern.size()) {
                    indexedPattern.erase(endTagPos, 1);
                }
            }
            lastNumberPos = endTagPos;

            ++numbersCount;
        }
    }

    for (unsigned int i = 0; i < indexes.size(); ++i) {
        ///check that all index is valid.
        if (indexes[i] >= numbersCount) {
            return false;
        }
    }

    *pattern = getPath() + indexedPattern;
    return true;
}


std::string removePath(std::string& filename) {

    ///find the last separator
    size_t pos = filename.find_last_of('/');
    if (pos == std::string::npos) {
        //try out \\ char
        pos = filename.find_last_of('\\');
    }
    if(pos == std::string::npos) {
        ///couldn't find a path
        return "";
    }
    std::string path = filename.substr(0,pos+1); // + 1 to include the trailing separator
    removeAllOccurences(filename, path,true);
    return path;
}


bool filesListFromPattern(const std::string& pattern,SequenceParsing::SequenceFromPattern* sequence) {
    if (pattern.empty()) {
        return false;
    }

    std::string patternUnPathed = pattern;
    std::string patternPath = removePath(patternUnPathed);
    std::string patternExtension = removeFileExtension(patternUnPathed);

    ///the pattern has no extension, switch the extension and the unpathed part
    if (patternUnPathed.empty()) {
        patternUnPathed = patternExtension;
        patternExtension.clear();
    }

    tinydir_dir patternDir;
    if (tinydir_open(&patternDir, patternPath.c_str()) == -1) {
        return false;
    }

    ///this list represents the common parts of the filename to find in a file in order for it to match the pattern.
    StringList commonPartsToFind;
    ///this list represents the variables ( ###  %04d %v etc...) found in the pattern ordered from left to right in the
    ///original string.
    std::vector< std::pair<std::string,int> > variablesByOrder;

    extractCommonPartsAndVariablesFromPattern(patternUnPathed, patternExtension, &commonPartsToFind, &variablesByOrder);


    ///all the interesting files of the pattern directory
    StringList files;
    getFilesFromDir(patternDir, &files);
    tinydir_close(&patternDir);

    for (int i = 0; i < (int)files.size(); ++i) {
        int frameNumber;
        int viewNumber;
        if (matchesPattern(files.at(i), commonPartsToFind, variablesByOrder, &frameNumber, &viewNumber)) {
            SequenceFromPattern::iterator it = sequence->find(frameNumber);
            std::string absoluteFileName = patternPath + files.at(i);
            if (it != sequence->end()) {
                std::pair<std::map<int,std::string>::iterator,bool> ret =
                        it->second.insert(std::make_pair(viewNumber,absoluteFileName));
                if (!ret.second) {
                    std::cerr << "There was an issue populating the file sequence. Several files with the same frame number"
                                 " have the same view index." << std::endl;
                }
            } else {
                std::map<int, std::string> viewsMap;
                viewsMap.insert(std::make_pair(viewNumber, absoluteFileName));
                sequence->insert(std::make_pair(frameNumber, viewsMap));
            }
        }
    }
    return true;
}

StringList sequenceFromPatternToFilesList(const SequenceParsing::SequenceFromPattern& sequence,int onlyViewIndex ) {
    StringList ret;
    for (SequenceParsing::SequenceFromPattern::const_iterator it = sequence.begin(); it!=sequence.end(); ++it) {
        const std::map<int,std::string>& views = it->second;

        for (std::map<int,std::string>::const_iterator it2 = views.begin(); it2!=views.end(); ++it2) {
            if (onlyViewIndex != -1 && it2->first != onlyViewIndex && it2->first != -1) {
                continue;
            }
            ret.push_back(it2->second);
        }
    }
    return ret;
}

std::string generateFileNameFromPattern(const std::string& pattern,int frameNumber,int viewNumber) {
    std::string patternUnPathed = pattern;
    std::string patternPath = removePath(patternUnPathed);
    std::string patternExtension = removeFileExtension(patternUnPathed);

    ///the pattern has no extension, switch the extension and the unpathed part
    if (patternUnPathed.empty()) {
        patternUnPathed = patternExtension;
        patternExtension.clear();
    }
    ///this list represents the common parts of the filename to find in a file in order for it to match the pattern.
    StringList commonPartsToFind;
    ///this list represents the variables ( ###  %04d %v etc...) found in the pattern ordered from left to right in the
    ///original string.
    std::vector<std::pair<std::string,int> > variablesByOrder;
    extractCommonPartsAndVariablesFromPattern(patternUnPathed, patternExtension, &commonPartsToFind, &variablesByOrder);

    std::string output = pattern;
    size_t lastVariablePos = std::string::npos;
    for (unsigned int i = 0; i < variablesByOrder.size(); ++i) {
        const std::string& variable = variablesByOrder[i].first;
        lastVariablePos = findStr(output, variable, lastVariablePos != std::string::npos ? lastVariablePos : 0);

        ///if we can't find the variable that means extractCommonPartsAndVariablesFromPattern is bugged.
        assert(lastVariablePos != std::string::npos);

        if (variable.find_first_of('#') != std::string::npos) {
            std::string frameNoStr = stringFromInt(frameNumber);
            ///prepend with extra 0's
            while (frameNoStr.size() < variable.size()) {
                frameNoStr.insert(0,1,'0');
            }
            output.replace(lastVariablePos, variable.size(), frameNoStr);
        } else if (variable.find("%v") != std::string::npos) {
            std::string viewNumberStr;
            if (viewNumber == 0) {
                viewNumberStr = "l";
            } else if (viewNumber == 1) {
                viewNumberStr = "r";
            } else {
                viewNumberStr = std::string("view") + stringFromInt(viewNumber);
            }

            output.replace(lastVariablePos,variable.size(), viewNumberStr);
        } else if (variable.find("%V") != std::string::npos) {
            std::string viewNumberStr;
            if (viewNumber == 0) {
                viewNumberStr = "left";
            } else if (viewNumber == 1) {
                viewNumberStr = "right";
            } else {
                viewNumberStr = std::string("view") + stringFromInt(viewNumber);
            }

            output.replace(lastVariablePos, variable.size(), viewNumberStr);
        } else if(startsWith(variable, "%0") && endsWith(variable,"d")) {
            std::string digitsCountStr = variable;
            removeAllOccurences(digitsCountStr,"%0");
            removeAllOccurences(digitsCountStr,"d");
            int digitsCount = stringToInt(digitsCountStr);
            std::string frameNoStr = stringFromInt(frameNumber);
            //prepend with extra 0's
            while ((int)frameNoStr.size() < digitsCount) {
                frameNoStr.insert(0,1,'0');
            }
            output.replace(lastVariablePos, variable.size(), frameNoStr);
        } else if (variable == "%d") {
            output.replace(lastVariablePos, variable.size(), stringFromInt(frameNumber));
        } else {
            throw std::invalid_argument("Unrecognized pattern: " + pattern);
        }
    }
    return output;
}

struct SequenceFromFilesPrivate
{
    /// the parsed files that have matching content with respect to variables.
    std::vector < FileNameContent > sequence;

    ///a list with all the files in the sequence, with their absolute file names.
    StringList filesList;

    ///all the files mapped to their index
    std::map<int,std::string> filesMap;

    /// The index of the frame number string in case there're several numbers in a filename.
    std::vector<int> frameNumberStringIndexes;

    unsigned long long totalSize;

    bool sizeEstimationEnabled;

    SequenceFromFilesPrivate(bool enableSizeEstimation)
        : sequence()
        , filesList()
        , filesMap()
        , frameNumberStringIndexes()
        , totalSize(0)
        , sizeEstimationEnabled(enableSizeEstimation)
    {

    }

    bool isInSequence(int index) const {
        return filesMap.find(index) != filesMap.end();
    }
};

SequenceFromFiles::SequenceFromFiles(bool enableSizeEstimation)
    : _imp(new SequenceFromFilesPrivate(enableSizeEstimation))
{

}

SequenceFromFiles::SequenceFromFiles(const FileNameContent& firstFile,  bool enableSizeEstimation)
    : _imp(new SequenceFromFilesPrivate(enableSizeEstimation))
{
    _imp->sequence.push_back(firstFile);
    _imp->filesList.push_back(firstFile.absoluteFileName());
    if (enableSizeEstimation) {
        std::ifstream file(firstFile.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
        _imp->totalSize += file.tellg();
    }
}

SequenceFromFiles::~SequenceFromFiles() {
    delete _imp;
}

SequenceFromFiles::SequenceFromFiles(const SequenceFromFiles& other)
    : _imp(new SequenceFromFilesPrivate(false))
{
    *this = other;
}

void SequenceFromFiles::operator=(const SequenceFromFiles& other) const {
    _imp->sequence = other._imp->sequence;
    _imp->filesList = other._imp->filesList;
    _imp->filesMap = other._imp->filesMap;
    _imp->frameNumberStringIndexes = other._imp->frameNumberStringIndexes;
    _imp->totalSize = other._imp->totalSize;
    _imp->sizeEstimationEnabled = other._imp->sizeEstimationEnabled;
}

bool SequenceFromFiles::tryInsertFile(const FileNameContent& file) {

    if (_imp->filesList.empty()) {
        _imp->sequence.push_back(file);
        _imp->filesList.push_back(file.absoluteFileName());
        if (_imp->sizeEstimationEnabled) {
            std::ifstream f(file.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
            _imp->totalSize += f.tellg();
        }
        return true;
    }

    if (file.getPath() != _imp->sequence[0].getPath()) {
        return false;
    }

    std::vector<int> frameNumberIndexes;
    bool insert = false;
    if (file.matchesPattern(_imp->sequence[0], &frameNumberIndexes)) {

        if (std::find(_imp->filesList.begin(),_imp->filesList.end(),file.absoluteFileName()) != _imp->filesList.end()) {
            return false;
        }

        if (_imp->frameNumberStringIndexes.empty()) {
            ///this is the second file we add to the sequence, we can now
            ///determine where is the frame number string placed.
            _imp->frameNumberStringIndexes = frameNumberIndexes;
            insert = true;

            ///insert the first frame number in the frameIndexes.
            std::string firstFrameNumberStr;

            for (unsigned int i = 0; i < frameNumberIndexes.size(); ++i) {
                std::string frameNumberStr;
                bool ok = _imp->sequence[0].getNumberByIndex(_imp->frameNumberStringIndexes[i], &frameNumberStr);
                if (ok && firstFrameNumberStr.empty()) {
                    _imp->filesMap.insert(std::make_pair(stringToInt(frameNumberStr),file.absoluteFileName()));
                    firstFrameNumberStr = frameNumberStr;
                } else if (!firstFrameNumberStr.empty() && stringToInt(frameNumberStr) != stringToInt(firstFrameNumberStr)) {
                    return false;
                }
            }


        } else if(frameNumberIndexes == _imp->frameNumberStringIndexes) {
            insert = true;
        }
        if (insert) {

            std::string firstFrameNumberStr;

            for (unsigned int i = 0; i < frameNumberIndexes.size(); ++i) {
                std::string frameNumberStr;
                bool ok = file.getNumberByIndex(_imp->frameNumberStringIndexes[i], &frameNumberStr);
                if (ok && firstFrameNumberStr.empty()) {
                    _imp->sequence.push_back(file);
                    _imp->filesList.push_back(file.absoluteFileName());
                    _imp->filesMap.insert(std::make_pair(stringToInt(frameNumberStr),file.absoluteFileName()));
                    if (_imp->sizeEstimationEnabled) {
                        std::ifstream f(file.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
                        _imp->totalSize += f.tellg();
                    }

                    firstFrameNumberStr = frameNumberStr;
                } else if (!firstFrameNumberStr.empty() && stringToInt(frameNumberStr) != stringToInt(firstFrameNumberStr)) {
                    return false;
                }
            }
        }
    }
    return insert;
}

bool SequenceFromFiles::contains(const std::string& absoluteFileName) const {
    return std::find(_imp->filesList.begin(),_imp->filesList.end(),absoluteFileName) != _imp->filesList.end();
}

bool SequenceFromFiles::empty() const {
    return _imp->filesList.empty();
}

int SequenceFromFiles::count() const {
    return (int)_imp->filesList.size();
}

bool SequenceFromFiles::isSingleFile() const {
    return _imp->sequence.size() == 1;
}

int SequenceFromFiles::getFirstFrame() const {
    if (_imp->filesMap.empty()) {
        return INT_MIN;
    } else {
        return _imp->filesMap.begin()->first;
    }
}

int SequenceFromFiles::getLastFrame() const {
    if (_imp->filesMap.empty()) {
        return INT_MAX;
    } else {
        std::map<int,std::string>::const_iterator it = _imp->filesMap.end();
        --it;
        return it->first;
    }
}

const std::map<int,std::string>& SequenceFromFiles::getFrameIndexes() const {
    return _imp->filesMap;
}

const StringList& SequenceFromFiles::getFilesList() const {
    return _imp->filesList;
}

unsigned long long SequenceFromFiles::getEstimatedTotalSize() const {
    return _imp->totalSize;
}

std::string SequenceFromFiles::generateValidSequencePattern() const
{
    if (empty()) {
        return "";
    }
    if (isSingleFile()) {
        return _imp->sequence[0].absoluteFileName();
    }
    assert(_imp->filesMap.size() >= 2);
    std::string firstFramePattern ;
    _imp->sequence[0].generatePatternWithFrameNumberAtIndexes(_imp->frameNumberStringIndexes, &firstFramePattern);
    return firstFramePattern;
}

std::string SequenceFromFiles::generateUserFriendlySequencePattern() const {
    if (isSingleFile()) {
        return _imp->sequence[0].fileName();
    }
    std::string pattern = generateValidSequencePattern();
    removePath(pattern);

    std::vector< std::pair<int,int> > chunks;
    int first = getFirstFrame();
    while(first <= getLastFrame()){

        int breakCounter = 0;
        while (!(_imp->isInSequence(first)) && breakCounter < NATRON_DIALOG_MAX_SEQUENCES_HOLE) {
            ++first;
            ++breakCounter;
        }

        if (breakCounter >= NATRON_DIALOG_MAX_SEQUENCES_HOLE) {
            break;
        }

        chunks.push_back(std::make_pair(first, getLastFrame()));
        int next = first + 1;
        int prev = first;
        int count = 1;
        while((next <= getLastFrame())
              && _imp->isInSequence(next)
              && (next == prev + 1) ){
            prev = next;
            ++next;
            ++count;
        }
        --next;
        chunks.back().second = next;
        first += count;
    }

    if (chunks.size() == 1) {
        pattern += ' ';
        pattern += stringFromInt(chunks[0].first);
        pattern += '-';
        pattern += stringFromInt(chunks[0].second);
    } else {
        pattern.append(" ( ");
        for(unsigned int i = 0 ; i < chunks.size() ; ++i) {
            if(chunks[i].first != chunks[i].second){
                pattern += ' ';
                pattern += stringFromInt(chunks[i].first);
                pattern += '-';
                pattern += stringFromInt(chunks[i].second);
            }else{
                pattern += ' ';
                pattern += stringFromInt(chunks[i].first);
            }
            if(i < chunks.size() -1) pattern.append(" /");
        }
        pattern.append(" ) ");
    }
    return pattern;
}

std::string SequenceFromFiles::fileExtension() const {
    if (!empty()) {
        return _imp->sequence[0].getExtension();
    } else {
        return "";
    }
}

std::string SequenceFromFiles::getPath() const {
    if (!empty()) {
        return _imp->sequence[0].getPath();
    } else {
        return "";
    }
}

bool SequenceFromFiles::getSequenceOutOfFile(const std::string& absoluteFileName,SequenceFromFiles* sequence)
{
    FileNameContent firstFile(absoluteFileName);
    sequence->tryInsertFile(firstFile);

    tinydir_dir dir;
    if (tinydir_open(&dir, firstFile.getPath().c_str()) == -1) {
        return false;
    }

    StringList allFiles;
    getFilesFromDir(dir, &allFiles);
    tinydir_close(&dir);

    for (StringList::iterator it = allFiles.begin(); it!=allFiles.end(); ++it) {
        sequence->tryInsertFile(FileNameContent(firstFile.getPath() + *it));
    }
    return true;
}

} // namespace SequenceParsing

