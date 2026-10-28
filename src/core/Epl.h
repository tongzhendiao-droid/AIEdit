#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 易语言 (EPL) source files (.e) and modules (.ec): what they declare, and their code as the
// IDE writes it out as text. After OpenEpl's EProjectFile (public domain), which documents the
// format; only reading, and only files without a password.
namespace aiedit::core::epl {

// Text in a file is in its own code page (GBK for Chinese, see Project::language); names and
// comments are kept as those bytes, and turned into UTF-8 only when written out.
using Bytes = std::string;

struct Library
{
    Bytes fileName; // "krnln", "iext", ...
    Bytes guid;
    Bytes name; // "系统核心支持库"
    int major = 0;
    int minor = 0;
};

struct Variable
{
    std::int32_t id = 0;
    std::int32_t dataType = 0;
    std::int16_t flags = 0;
    std::vector<std::int32_t> bounds; // of an array: each dimension, 0 for one not fixed
    Bytes name;
    Bytes comment;
};

struct Method
{
    std::int32_t id = 0;
    std::int32_t classId = 0;
    std::int32_t flags = 0;
    std::int32_t returnType = 0;
    Bytes name;
    Bytes comment;
    std::vector<Variable> locals;
    std::vector<Variable> parameters;
    Bytes code; // the statements, as the IDE stores them
};

struct Class
{
    std::int32_t id = 0;
    std::int32_t form = 0;
    std::int32_t baseClass = 0;
    bool isPublic = false;
    Bytes name;
    Bytes comment;
    std::vector<std::int32_t> methods;
    std::vector<Variable> variables;
};

struct Struct
{
    std::int32_t id = 0;
    std::int32_t flags = 0;
    Bytes name;
    Bytes comment;
    std::vector<Variable> members;
};

struct Dll
{
    std::int32_t id = 0;
    std::int32_t flags = 0;
    std::int32_t returnType = 0;
    Bytes name;
    Bytes comment;
    Bytes library;
    Bytes entryPoint;
    std::vector<Variable> parameters;
};

struct Constant
{
    enum class Kind
    {
        Empty,
        Number,
        Boolean,
        Date,
        Text,
        Image, // a picture resource
        Sound, // a sound resource
    };
    std::int32_t id = 0;
    std::int16_t flags = 0;
    Bytes name;
    Bytes comment;
    Kind kind = Kind::Empty;
    double number = 0; // a number, a boolean (0 or 1), or a date (OLE: days since 1899-12-30)
    Bytes text;
    std::size_t resourceSize = 0;
};

struct FormElement
{
    std::int32_t id = 0;
    std::int32_t dataType = 0;
    Bytes name;
};

struct Form
{
    std::int32_t id = 0;
    std::int32_t classId = 0;
    Bytes name;
    Bytes comment;
    std::vector<FormElement> elements;
};

struct Project
{
    enum class Status
    {
        Ok,
        Encrypted, // has a password: not read
        Broken,    // not read to the end; what came before is kept
        NotEpl,
    };
    Status status = Status::NotEpl;
    int language = 1; // 1 Chinese (GBK), 2 English, 3 Chinese (Big5), 4 Japanese (Shift-JIS)
    int fileType = 1; // 1 a source file, 3 a module
    std::vector<Library> libraries;
    std::vector<Class> classes;
    std::vector<Method> methods;
    std::vector<Variable> globals;
    std::vector<Struct> structs;
    std::vector<Dll> dlls;
    std::vector<Constant> constants;
    std::vector<Form> forms;
    // Names of things removed (or made anew) whose ids code may still refer to: the IDE keeps
    // them, and shows code with them.
    std::vector<std::pair<std::int32_t, Bytes>> removedNames;
    std::int32_t mainMethod = 0;
};

Project parse(std::span<const std::uint8_t> data);

// Text in the file's code page to UTF-8.
using Decoder = std::function<std::string(std::string_view)>;

// The project as the IDE writes it out as text (.版本 2, .程序集, .子程序, ...), in UTF-8. Names
// of library commands, types and constants that are not known here are written as
// 〈krnln命令123〉 and the like. `unreadable` counts methods whose code could not all be read.
std::string toText(const Project& project, const Decoder& decode, std::size_t* unreadable = nullptr);

} // namespace aiedit::core::epl
