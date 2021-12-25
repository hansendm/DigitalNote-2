//------------------------------------------------------------------------------
// Tinyformat: A minimal type safe printf replacement
//
// tinyformat.h is a type safe printf replacement library in a single C++
// header file.  Design goals include:
//
// * Type safety and extensibility for user defined types.
// * C99 printf() compatibility, to the extent possible using std::ostream
// * Simplicity and minimalism.  A single header file to include and distribute
//   with your projects.
// * Augment rather than replace the standard stream formatting mechanism
// * C++98 support, with optional C++11 niceties
//
//
// Main interface example usage
// ----------------------------
//
// To print a date to std::cout:
//
//   std::string weekday = "Wednesday";
//   const char* month = "July";
//   size_t day = 27;
//   long hour = 14;
//   int min = 44;
//
//   tfm::printf("%s, %s %d, %.2d:%.2d\n", weekday, month, day, hour, min);
//
// The strange types here emphasize the type safety of the interface; it is
// possible to print a std::string using the "%s" conversion, and a
// size_t using the "%d" conversion.  A similar result could be achieved
// using either of the tfm::format() functions.  One prints on a user provided
// stream:
//
//   tfm::format(std::cerr, "%s, %s %d, %.2d:%.2d\n",
//               weekday, month, day, hour, min);
//
// The other returns a std::string:
//
//   std::string date = tfm::format("%s, %s %d, %.2d:%.2d\n",
//                                  weekday, month, day, hour, min);
//   std::cout << date;
//
// These are the three primary interface functions.
//
//
// User defined format functions
// -----------------------------
//
// Simulating variadic templates in C++98 is pretty painful since it requires
// writing out the same function for each desired number of arguments.  To make
// this bearable tinyformat comes with a set of macros which are used
// internally to generate the API, but which may also be used in user code.
//
// The three macros TINYFORMAT_ARGTYPES(n), TINYFORMAT_VARARGS(n) and
// TINYFORMAT_PASSARGS(n) will generate a list of n argument types,
// type/name pairs and argument names respectively when called with an integer
// n between 1 and 16.  We can use these to define a macro which generates the
// desired user defined function with n arguments.  To generate all 16 user
// defined function bodies, use the macro TINYFORMAT_FOREACH_ARGNUM.  For an
// example, see the implementation of printf() at the end of the source file.
//
//
// Additional API information
// --------------------------
//
// Error handling: Define TINYFORMAT_ERROR to customize the error handling for
// format strings which are unsupported or have the wrong number of format
// specifiers (calls assert() by default).
//
// User defined types: Uses operator<< for user defined types by default.
// Overload formatValue() for more control.


#ifndef TINYFORMAT_H_INCLUDED
#define TINYFORMAT_H_INCLUDED

namespace tinyformat {}
//------------------------------------------------------------------------------
// Config section.  Customize to your liking!

// Namespace alias to encourage brevity
namespace tfm = tinyformat;

// Error handling; calls assert() by default.
#define TINYFORMAT_ERROR(reasonString) throw std::runtime_error(reasonString)

// Define for C++11 variadic templates which make the code shorter & more
// general.  If you don't define this, C++11 support is autodetected below.
// #define TINYFORMAT_USE_VARIADIC_TEMPLATES


//------------------------------------------------------------------------------
// Implementation details.
#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifndef TINYFORMAT_ERROR
#   define TINYFORMAT_ERROR(reason) assert(0 && reason)
#endif

#if !defined(TINYFORMAT_USE_VARIADIC_TEMPLATES) && !defined(TINYFORMAT_NO_VARIADIC_TEMPLATES)
#   ifdef __GXX_EXPERIMENTAL_CXX0X__
#       define TINYFORMAT_USE_VARIADIC_TEMPLATES
#   endif
#endif

#ifdef __GNUC__
#   define TINYFORMAT_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#   define TINYFORMAT_NOINLINE __declspec(noinline)
#else
#   define TINYFORMAT_NOINLINE
#endif

#if defined(__GLIBCXX__) && __GLIBCXX__ < 20080201
//  std::showpos is broken on old libstdc++ as provided with OSX.  See
//  http://gcc.gnu.org/ml/libstdc++/2007-11/msg00075.html
#   define TINYFORMAT_OLD_LIBSTDCPLUSPLUS_WORKAROUND
#endif

namespace tinyformat {

//------------------------------------------------------------------------------
namespace detail {

// Test whether type T1 is convertible to type T2
template <typename T1, typename T2>
struct is_convertible
{
    private:
        // two types of different size
        struct fail { char dummy[2]; };
        struct succeed { char dummy; };
        // Try to convert a T1 to a T2 by plugging into tryConvert
        static fail tryConvert(...);
        static succeed tryConvert(const T2&);
        static const T1& makeT1();
    public:
#       ifdef _MSC_VER
        // Disable spurious loss of precision warnings in tryConvert(makeT1())
#       pragma warning(push)
#       pragma warning(disable:4244)
#       pragma warning(disable:4267)
#       endif
        // Standard trick: the (...) version of tryConvert will be chosen from
        // the overload set only if the version taking a T2 doesn't match.
        // Then we compare the sizes of the return types to check which
        // function matched.  Very neat, in a disgusting kind of way :)
        static const bool value =
            sizeof(tryConvert(makeT1())) == sizeof(succeed);
#       ifdef _MSC_VER
#       pragma warning(pop)
#       endif
};


// Detect when a type is not a wchar_t string
template<typename T> struct is_wchar { typedef int tinyformat_wchar_is_not_supported; };
template<> struct is_wchar<wchar_t*> {};
template<> struct is_wchar<const wchar_t*> {};
template<int n> struct is_wchar<const wchar_t[n]> {};
template<int n> struct is_wchar<wchar_t[n]> {};


// Format the value by casting to type fmtT.  This default implementation
// should never be called.
template<typename T, typename fmtT, bool convertible = is_convertible<T, fmtT>::value>
struct formatValueAsType
{
    static void invoke(std::ostream& /*out*/, const T& /*value*/) { assert(0); }
};
// Specialized version for types that can actually be converted to fmtT, as
// indicated by the "convertible" template parameter.
template<typename T, typename fmtT>
struct formatValueAsType<T,fmtT,true>
{
    static void invoke(std::ostream& out, const T& value)
        { out << static_cast<fmtT>(value); }
};

#ifdef TINYFORMAT_OLD_LIBSTDCPLUSPLUS_WORKAROUND
template<typename T, bool convertible = is_convertible<T, int>::value>
struct formatZeroIntegerWorkaround
{
    static bool invoke(std::ostream& /**/, const T& /**/) { return false; }
};
template<typename T>
struct formatZeroIntegerWorkaround<T,true>
{
    static bool invoke(std::ostream& out, const T& value)
    {
        if (static_cast<int>(value) == 0 && out.flags() & std::ios::showpos)
        {
            out << "+0";
            return true;
        }
        return false;
    }
};
#endif // TINYFORMAT_OLD_LIBSTDCPLUSPLUS_WORKAROUND

// Convert an arbitrary type to integer.  The version with convertible=false
// throws an error.
template<typename T, bool convertible = is_convertible<T,int>::value>
struct convertToInt
{
    static int invoke(const T& /*value*/)
    {
        TINYFORMAT_ERROR("tinyformat: Cannot convert from argument type to "
                         "integer for use as variable width or precision");
        return 0;
    }
};
// Specialization for convertToInt when conversion is possible
template<typename T>
struct convertToInt<T,true>
{
    static int invoke(const T& value) { return static_cast<int>(value); }
};

} // namespace detail


//------------------------------------------------------------------------------
// Variable formatting functions.  May be overridden for user-defined types if
// desired.


// Format a value into a stream. Called from format() for all types by default.
//
// Users may override this for their own types.  When this function is called,
// the stream flags will have been modified according to the format string.
// The format specification is provided in the range [fmtBegin, fmtEnd).
//
// By default, formatValue() uses the usual stream insertion operator
// operator<< to format the type T, with special cases for the %c and %p
// conversions.
template<typename T>
inline void formatValue(std::ostream& out, const char* /*fmtBegin*/,
                        const char* fmtEnd, const T& value)
{
#ifndef TINYFORMAT_ALLOW_WCHAR_STRINGS
    // Since we don't support printing of wchar_t using "%ls", make it fail at
    // compile time in preference to printing as a void* at runtime.
    typedef typename detail::is_wchar<T>::tinyformat_wchar_is_not_supported DummyType;
    (void) DummyType(); // avoid unused type warning with gcc-4.8
#endif
    // The mess here is to support the %c and %p conversions: if these
    // conversions are active we try to convert the type to a char or const
    // void* respectively and format that instead of the value itself.  For the
    // %p conversion it's important to avoid dereferencing the pointer, which
    // could otherwise lead to a crash when printing a dangling (const char*).
    const bool canConvertToChar = detail::is_convertible<T,char>::value;
    const bool canConvertToVoidPtr = detail::is_convertible<T, const void*>::value;
    if(canConvertToChar && *(fmtEnd-1) == 'c')
        detail::formatValueAsType<T, char>::invoke(out, value);
    else if(canConvertToVoidPtr && *(fmtEnd-1) == 'p')
        detail::formatValueAsType<T, const void*>::invoke(out, value);
#ifdef TINYFORMAT_OLD_LIBSTDCPLUSPLUS_WORKAROUND
    else if(detail::formatZeroIntegerWorkaround<T>::invoke(out, value)) /**/;
#endif
    else
        out << value;
}


// Overloaded version for char types to support printing as an integer
#define TINYFORMAT_DEFINE_FORMATVALUE_CHAR(charType)                  \
inline void formatValue(std::ostream& out, const char* /*fmtBegin*/,  \
                        const char* fmtEnd, charType value)           \
{                                                                     \
    switch(*(fmtEnd-1))                                               \
    {                                                                 \
        case 'u': case 'd': case 'i': case 'o': case 'X': case 'x':   \
            out << static_cast<int>(value); break;                    \
        default:                                                      \
            out << value;                   break;                    \
    }                                                                 \
}
// per 3.9.1: char, signed char and unsigned char are all distinct types
TINYFORMAT_DEFINE_FORMATVALUE_CHAR(char)
TINYFORMAT_DEFINE_FORMATVALUE_CHAR(signed char)
TINYFORMAT_DEFINE_FORMATVALUE_CHAR(unsigned char)
#undef TINYFORMAT_DEFINE_FORMATVALUE_CHAR


//------------------------------------------------------------------------------
// Tools for emulating variadic templates in C++98.  The basic idea here is
// stolen from the boost preprocessor metaprogramming library and cut down to
// be just general enough for what we need.

#define TINYFORMAT_ARGTYPES(n) TINYFORMAT_ARGTYPES_ ## n
#define TINYFORMAT_VARARGS(n) TINYFORMAT_VARARGS_ ## n
#define TINYFORMAT_PASSARGS(n) TINYFORMAT_PASSARGS_ ## n
#define TINYFORMAT_PASSARGS_TAIL(n) TINYFORMAT_PASSARGS_TAIL_ ## n

// To keep it as transparent as possible, the macros below have been generated
// using python via the excellent cog.py code generation script.  This avoids
// the need for a bunch of complex (but more general) preprocessor tricks as
// used in boost.preprocessor.
//
// To rerun the code generation in place, use `cog.py -r tinyformat.h`
// (see http://nedbatchelder.com/code/cog).  Alternatively you can just create
// extra versions by hand.

/*[[[cog
maxParams = 16

def makeCommaSepLists(lineTemplate, elemTemplate, startInd=1):
    for j in range(startInd,maxParams+1):
        list = ', '.join([elemTemplate % {'i':i} for i in range(startInd,j+1)])
        cog.outl(lineTemplate % {'j':j, 'list':list})

makeCommaSepLists('#define TINYFORMAT_ARGTYPES_%(j)d %(list)s',
                  'class T%(i)d')

cog.outl()
makeCommaSepLists('#define TINYFORMAT_VARARGS_%(j)d %(list)s',
                  'const T%(i)d& v%(i)d')

cog.outl()
makeCommaSepLists('#define TINYFORMAT_PASSARGS_%(j)d %(list)s', 'v%(i)d')

cog.outl()
cog.outl('#define TINYFORMAT_PASSARGS_TAIL_1')
makeCommaSepLists('#define TINYFORMAT_PASSARGS_TAIL_%(j)d , %(list)s',
                  'v%(i)d', startInd = 2)

cog.outl()
cog.outl('#define TINYFORMAT_FOREACH_ARGNUM(m) \\\n    ' +
         ' '.join(['m(%d)' % (j,) for j in range(1,maxParams+1)]))
]]]*/
#define TINYFORMAT_ARGTYPES_1 class T1
#define TINYFORMAT_ARGTYPES_2 class T1, class T2
#define TINYFORMAT_ARGTYPES_3 class T1, class T2, class T3
#define TINYFORMAT_ARGTYPES_4 class T1, class T2, class T3, class T4
#define TINYFORMAT_ARGTYPES_5 class T1, class T2, class T3, class T4, class T5
#define TINYFORMAT_ARGTYPES_6 class T1, class T2, class T3, class T4, class T5, class T6
#define TINYFORMAT_ARGTYPES_7 class T1, class T2, class T3, class T4, class T5, class T6, class T7
#define TINYFORMAT_ARGTYPES_8 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8
#define TINYFORMAT_ARGTYPES_9 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9
#define TINYFORMAT_ARGTYPES_10 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10
#define TINYFORMAT_ARGTYPES_11 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class T11
#define TINYFORMAT_ARGTYPES_12 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class T11, class T12
#define TINYFORMAT_ARGTYPES_13 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class T11, class T12, class T13
#define TINYFORMAT_ARGTYPES_14 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class T11, class T12, class T13, class T14
#define TINYFORMAT_ARGTYPES_15 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class T11, class T12, class T13, class T14, class T15
#define TINYFORMAT_ARGTYPES_16 class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class T11, class T12, class T13, class T14, class T15, class T16

#define TINYFORMAT_VARARGS_1 const T1& v1
#define TINYFORMAT_VARARGS_2 const T1& v1, const T2& v2
#define TINYFORMAT_VARARGS_3 const T1& v1, const T2& v2, const T3& v3
#define TINYFORMAT_VARARGS_4 const T1& v1, const T2& v2, const T3& v3, const T4& v4
#define TINYFORMAT_VARARGS_5 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5
#define TINYFORMAT_VARARGS_6 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6
#define TINYFORMAT_VARARGS_7 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7
#define TINYFORMAT_VARARGS_8 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8
#define TINYFORMAT_VARARGS_9 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9
#define TINYFORMAT_VARARGS_10 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10
#define TINYFORMAT_VARARGS_11 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10, const T11& v11
#define TINYFORMAT_VARARGS_12 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10, const T11& v11, const T12& v12
#define TINYFORMAT_VARARGS_13 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10, const T11& v11, const T12& v12, const T13& v13
#define TINYFORMAT_VARARGS_14 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10, const T11& v11, const T12& v12, const T13& v13, const T14& v14
#define TINYFORMAT_VARARGS_15 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10, const T11& v11, const T12& v12, const T13& v13, const T14& v14, const T15& v15
#define TINYFORMAT_VARARGS_16 const T1& v1, const T2& v2, const T3& v3, const T4& v4, const T5& v5, const T6& v6, const T7& v7, const T8& v8, const T9& v9, const T10& v10, const T11& v11, const T12& v12, const T13& v13, const T14& v14, const T15& v15, const T16& v16

#define TINYFORMAT_PASSARGS_1 v1
#define TINYFORMAT_PASSARGS_2 v1, v2
#define TINYFORMAT_PASSARGS_3 v1, v2, v3
#define TINYFORMAT_PASSARGS_4 v1, v2, v3, v4
#define TINYFORMAT_PASSARGS_5 v1, v2, v3, v4, v5
#define TINYFORMAT_PASSARGS_6 v1, v2, v3, v4, v5, v6
#define TINYFORMAT_PASSARGS_7 v1, v2, v3, v4, v5, v6, v7
#define TINYFORMAT_PASSARGS_8 v1, v2, v3, v4, v5, v6, v7, v8
#define TINYFORMAT_PASSARGS_9 v1, v2, v3, v4, v5, v6, v7, v8, v9
#define TINYFORMAT_PASSARGS_10 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10
#define TINYFORMAT_PASSARGS_11 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11
#define TINYFORMAT_PASSARGS_12 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12
#define TINYFORMAT_PASSARGS_13 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13
#define TINYFORMAT_PASSARGS_14 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14
#define TINYFORMAT_PASSARGS_15 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15
#define TINYFORMAT_PASSARGS_16 v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16

#define TINYFORMAT_PASSARGS_TAIL_1
#define TINYFORMAT_PASSARGS_TAIL_2 , v2
#define TINYFORMAT_PASSARGS_TAIL_3 , v2, v3
#define TINYFORMAT_PASSARGS_TAIL_4 , v2, v3, v4
#define TINYFORMAT_PASSARGS_TAIL_5 , v2, v3, v4, v5
#define TINYFORMAT_PASSARGS_TAIL_6 , v2, v3, v4, v5, v6
#define TINYFORMAT_PASSARGS_TAIL_7 , v2, v3, v4, v5, v6, v7
#define TINYFORMAT_PASSARGS_TAIL_8 , v2, v3, v4, v5, v6, v7, v8
#define TINYFORMAT_PASSARGS_TAIL_9 , v2, v3, v4, v5, v6, v7, v8, v9
#define TINYFORMAT_PASSARGS_TAIL_10 , v2, v3, v4, v5, v6, v7, v8, v9, v10
#define TINYFORMAT_PASSARGS_TAIL_11 , v2, v3, v4, v5, v6, v7, v8, v9, v10, v11
#define TINYFORMAT_PASSARGS_TAIL_12 , v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12
#define TINYFORMAT_PASSARGS_TAIL_13 , v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13
#define TINYFORMAT_PASSARGS_TAIL_14 , v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14
#define TINYFORMAT_PASSARGS_TAIL_15 , v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15
#define TINYFORMAT_PASSARGS_TAIL_16 , v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16

#define TINYFORMAT_FOREACH_ARGNUM(m) \
    m(1) m(2) m(3) m(4) m(5) m(6) m(7) m(8) m(9) m(10) m(11) m(12) m(13) m(14) m(15) m(16)
//[[[end]]]



namespace detail {

// Class holding current position in format string and an output stream into
// which arguments are formatted.
class FormatIterator
{
    public:
        // Flags for features not representable with standard stream state
        enum ExtraFormatFlags
        {
            Flag_None                = 0,
            Flag_TruncateToPrecision = 1<<0, // truncate length to stream precision()
            Flag_SpacePadPositive    = 1<<1, // pad positive values with spaces
            Flag_VariableWidth       = 1<<2, // variable field width in arg list
            Flag_VariablePrecision   = 1<<3  // variable field precision in arg list
        };

        // out is the output stream, fmt is the full format string
        FormatIterator(std::ostream& out, const char* fmt)
            : m_out(out),
            m_fmt(fmt),
            m_extraFlags(Flag_None),
            m_wantWidth(false),
            m_wantPrecision(false),
            m_variableWidth(0),
            m_variablePrecision(0),
            m_origWidth(out.width()),
            m_origPrecision(out.precision()),
            m_origFlags(out.flags()),
            m_origFill(out.fill())
        { }

        // Print remaining part of format string.
        void finish()
        {
            // It would be nice if we could do this from the destructor, but we
            // can't if TINFORMAT_ERROR is used to throw an exception!
            m_fmt = printFormatStringLiteral(m_out, m_fmt);
            if(*m_fmt != '\0')
                TINYFORMAT_ERROR("tinyformat: Too many conversion specifiers in format string");
        }

        ~FormatIterator()
        {
            // Restore stream state
            m_out.width(m_origWidth);
            m_out.precision(m_origPrecision);
            m_out.flags(m_origFlags);
            m_out.fill(m_origFill);
        }

        template<typename T>
        void accept(const T& value);

    private:
        // Parse and return an integer from the string c, as atoi()
        // On return, c is set to one past the end of the integer.
        static int parseIntAndAdvance(const char*& c)
        {
            int i = 0;
            for(;*c >= '0' && *c <= '9'; ++c)
                i = 10*i + (*c - '0');
            return i;
        }

        // Format at most truncLen characters of a C string to the given
        // stream.  Return true if formatting proceeded (generic version always
        // returns false)
        template<typename T>
        static bool formatCStringTruncate(std::ostream& /*out*/, const T& /*value*/,
                                        std::streamsize /*truncLen*/)
        {
            return false;
        }
#       define TINYFORMAT_DEFINE_FORMAT_C_STRING_TRUNCATE(type)            \
        static bool formatCStringTruncate(std::ostream& out, type* value,  \
                                        std::streamsize truncLen)          \
        {                                                                  \
            std::streamsize len = 0;                                       \
            while(len < truncLen && value[len] != 0)                       \
                ++len;                                                     \
            out.write(value, len);                                         \
            return true;                                                   \
        }
        // Overload for const char* and char*.  Could overload for signed &
        // unsigned char too, but these are technically unneeded for printf
        // compatibility.
        TINYFORMAT_DEFINE_FORMAT_C_STRING_TRUNCATE(const char)
        TINYFORMAT_DEFINE_FORMAT_C_STRING_TRUNCATE(char)
#       undef TINYFORMAT_DEFINE_FORMAT_C_STRING_TRUNCATE

        // Print literal part of format string and return next format spec
        // position.
        //
        // Skips over any occurrences of '%%', printing a literal '%' to the
        // output.  The position of the first % character of the next
        // nontrivial format spec is returned, or the end of string.
        static const char* printFormatStringLiteral(std::ostream& out,
                                                    const char* fmt)
        {
            const char* c = fmt;
            for(; true; ++c)
            {
                switch(*c)
                {
                    case '\0':
                        out.write(fmt, static_cast<std::streamsize>(c - fmt));
                        return c;
                    case '%':
                        out.write(fmt, static_cast<std::streamsize>(c - fmt));
                        if(*(c+1) != '%')
                            return c;
                        // for "%%", tack trailing % onto next literal section.
                        fmt = ++c;
                        break;
                }
            }
        }

        static const char* streamStateFromFormat(std::ostream& out,
                                                 unsigned int& extraFlags,
                                                 const char* fmtStart,
                                                 int variableWidth,
                                                 int variablePrecision);

        // Private copy & assign: Kill gcc warnings with -Weffc++
        FormatIterator(const FormatIterator&);
        FormatIterator& operator=(const FormatIterator&);

        // Stream, current format string & state
        std::ostream& m_out;
        const char* m_fmt;
        unsigned int m_extraFlags;
        // State machine info for handling of variable width & precision
        bool m_wantWidth;
        bool m_wantPrecision;
        int m_variableWidth;
        int m_variablePrecision;
        // Saved stream state
        std::streamsize m_origWidth;
        std::streamsize m_origPrecision;
        std::ios::fmtflags m_origFlags;
        char m_origFill;
};


// Accept a value for formatting into the internal stream.
template<typename T>
TINYFORMAT_NOINLINE  // < greatly reduces bloat in optimized builds
void FormatIterator::accept(const T& value)
{
    // Parse the format string
    const char* fmtEnd = 0;
    if(m_extraFlags == Flag_None && !m_wantWidth && !m_wantPrecision)
    {
        m_fmt = printFormatStringLiteral(m_out, m_fmt);
        fmtEnd = streamStateFromFormat(m_out, m_extraFlags, m_fmt, 0, 0);
        m_wantWidth     = (m_extraFlags & Flag_VariableWidth) != 0;
        m_wantPrecision = (m_extraFlags & Flag_VariablePrecision) != 0;
    }
    // Consume value as variable width and precision specifier if necessary
    if(m_extraFlags & (Flag_VariableWidth | Flag_VariablePrecision))
    {
        if(m_wantWidth || m_wantPrecision)
        {
            int v = convertToInt<T>::invoke(value);
            if(m_wantWidth)
            {
                m_variableWidth = v;
                m_wantWidth = false;
            }
            else if(m_wantPrecision)
            {
                m_variablePrecision = v;
                m_wantPrecision = false;
            }
            return;
        }
        // If we get here, we've set both the variable precision and width as
        // required and we need to rerun the stream state setup to insert these.
        fmtEnd = streamStateFromFormat(m_out, m_extraFlags, m_fmt,
                                       m_variableWidth, m_variablePrecision);
    }

    // Format the value into the stream.
    if(!(m_extraFlags & (Flag_SpacePadPositive | Flag_TruncateToPrecision)))
        formatValue(m_out, m_fmt, fmtEnd, value);
    else
    {
        // The following are special cases where there's no direct
        // correspondence between stream formatting and the printf() behaviour.
        // Instead, we simulate the behaviour crudely by formatting into a
        // temporary string stream and munging the resulting string.
        std::ostringstream tmpStream;
        tmpStream.copyfmt(m_out);
        if(m_extraFlags & Flag_SpacePadPositive)
            tmpStream.setf(std::ios::showpos);
        // formatCStringTruncate is required for truncating conversions like
        // "%.4s" where at most 4 characters of the c-string should be read.
        // If we didn't include this special case, we might read off the end.
        if(!( (m_extraFlags & Flag_TruncateToPrecision) &&
             formatCStringTruncate(tmpStream, value, m_out.precision()) ))
        {
            // Not a truncated c-string; just format normally.
            formatValue(tmpStream, m_fmt, fmtEnd, value);
        }
        std::string result = tmpStream.str(); // allocates... yuck.
        if(m_extraFlags & Flag_SpacePadPositive)
        {
            for(size_t i = 0, iend = result.size(); i < iend; ++i)
                if(result[i] == '+')
                    result[i] = ' ';
        }
        if((m_extraFlags & Flag_TruncateToPrecision) &&
           (int)result.size() > (int)m_out.precision())
            m_out.write(result.c_str(), m_out.precision());
        else
            m_out << result;
    }
    m_extraFlags = Flag_None;
    m_fmt = fmtEnd;
}


// Parse a format string and set the stream state accordingly.
//
// The format mini-language recognized here is meant to be the one from C99,
// with the form "%[flags][width][.precision][length]type".
//
// Formatting options which can't be natively represented using the ostream
// state are returned in the extraFlags parameter which is a bitwise
// combination of values from the ExtraFormatFlags enum.
inline const char* FormatIterator::streamStateFromFormat(std::ostream& out,
                                                         unsigned int& extraFlags,
                                                         const char* fmtStart,
                                                         int variableWidth,
                                                         int variablePrecision)
{
    if(*fmtStart != '%')
    {
        TINYFORMAT_ERROR("tinyformat: Not enough conversion specifiers in format string");
        return fmtStart;
    }
    // Reset stream state to defaults.
    out.width(0);
    out.precision(6);
    out.fill(' ');
    // Reset most flags; ignore irrelevant unitbuf & skipws.
    out.unsetf(std::ios::adjustfield | std::ios::basefield |
               std::ios::floatfield | std::ios::showbase | std::ios::boolalpha |
               std::ios::showpoint | std::ios::showpos | std::ios::uppercase);
    extraFlags = Flag_None;
    bool precisionSet = false;
    bool widthSet = false;
    const char* c = fmtStart + 1;
    // 1) Parse flags
    for(;; ++c)
    {
        switch(*c)
        {
            case '#':
                out.setf(std::ios::showpoint | std::ios::showbase);
                continue;
            case '0':
                // overridden by left alignment ('-' flag)
                if(!(out.flags() & std::ios::left))
                {
                    // Use internal padding so that numeric values are
                    // formatted correctly, eg -00010 rather than 000-10
                    out.fill('0');
                    out.setf(std::ios::internal, std::ios::adjustfield);
                }
                continue;
            case '-':
                out.fill(' ');
                out.setf(std::ios::left, std::ios::adjustfield);
                continue;
            case ' ':
                // overridden by show positive sign, '+' flag.
                if(!(out.flags() & std::ios::showpos))
                    extraFlags |= Flag_SpacePadPositive;
                continue;
            case '+':
                out.setf(std::ios::showpos);
                extraFlags &= ~Flag_SpacePadPositive;
                continue;
        }
        break;
    }
    // 2) Parse width
    if(*c >= '0' && *c <= '9')
    {
        widthSet = true;
        out.width(parseIntAndAdvance(c));
    }
    if(*c == '*')
    {
        widthSet = true;
        if(variableWidth < 0)
        {
            // negative widths correspond to '-' flag set
            out.fill(' ');
            out.setf(std::ios::left, std::ios::adjustfield);
            variableWidth = -variableWidth;
        }
        out.width(variableWidth);
        extraFlags |= Flag_VariableWidth;
        ++c;
    }
    // 3) Parse precision
    if(*c == '.')
    {
        ++c;
        int precision = 0;
        if(*c == '*')
        {
            ++c;
            extraFlags |= Flag_VariablePrecision;
            precision = variablePrecision;
        }
        else
        {
            if(*c >= '0' && *c <= '9')
                precision = parseIntAndAdvance(c);
            else if(*c == '-') // negative precisions ignored, treated as zero.
                parseIntAndAdvance(++c);
        }
        out.precision(precision);
        precisionSet = true;
    }
    // 4) Ignore any C99 length modifier
    while(*c == 'l' || *c == 'h' || *c == 'L' ||
          *c == 'j' || *c == 'z' || *c == 't')
        ++c;
    // 5) We're up to the conversion specifier character.
    // Set stream flags based on conversion specifier (thanks to the
    // boost::format class for forging the way here).
    bool intConversion = false;
    switch(*c)
    {
        case 'u': case 'd': case 'i':
            out.setf(std::ios::dec, std::ios::basefield);
            intConversion = true;
            break;
        case 'o':
            out.setf(std::ios::oct, std::ios::basefield);
            intConversion = true;
            break;
        case 'X':
            out.setf(std::ios::uppercase);
            break;
        case 'x': case 'p':
            out.setf(std::ios::hex, std::ios::basefield);
            intConversion = true;
            break;
        case 'E':
            out.setf(std::ios::uppercase);
            break;
        case 'e':
            out.setf(std::ios::scientific, std::ios::floatfield);
            out.setf(std::ios::dec, std::ios::basefield);
            break;
        case 'F':
            out.setf(std::ios::uppercase);
            break;
        case 'f':
            out.setf(std::ios::fixed, std::ios::floatfield);
            break;
        case 'G':
            out.setf(std::ios::uppercase);
            break;
        case 'g':
            out.setf(std::ios::dec, std::ios::basefield);
            // As in boost::format, let stream decide float format.
            out.flags(out.flags() & ~std::ios::floatfield);
            break;
        case 'a': case 'A':
            TINYFORMAT_ERROR("tinyformat: the %a and %A conversion specs "
                             "are not supported");
            break;
        case 'c':
            // Handled as special case inside formatValue()
            break;
        case 's':
            if(precisionSet)
                extraFlags |= Flag_TruncateToPrecision;
            // Make %s print booleans as "true" and "false"
            out.setf(std::ios::boolalpha);
            break;
        case 'n':
            // Not supported - will cause problems!
            TINYFORMAT_ERROR("tinyformat: %n conversion spec not supported");
            break;
        case '\0':
            TINYFORMAT_ERROR("tinyformat: Conversion spec incorrectly "
                             "terminated by end of string");
            return c;
    }
    if(intConversion && precisionSet && !widthSet)
    {
        // "precision" for integers gives the minimum number of digits (to be
        // padded with zeros on the left).  This isn't really supported by the
        // iostreams, but we can approximately simulate it with the width if
        // the width isn't otherwise used.
        out.width(out.precision());
        out.setf(std::ios::internal, std::ios::adjustfield);
        out.fill('0');
    }
    return c+1;
}



//------------------------------------------------------------------------------
// Private format function on top of which the public interface is implemented.
// We enforce a mimimum of one value to be formatted to prevent bugs looking like
//
//   const char* myStr = "100% broken";
//   printf(myStr);   // Parses % as a format specifier
#ifdef TINYFORMAT_USE_VARIADIC_TEMPLATES

template<typename T1>
void format(FormatIterator& fmtIter, const T1& value1)
{
    fmtIter.accept(value1);
    fmtIter.finish();
}

// General version for C++11
template<typename T1, typename... Args>
void format(FormatIterator& fmtIter, const T1& value1, const Args&... args)
{
    fmtIter.accept(value1);
    format(fmtIter, args...);
}

#else

inline void format(FormatIterator& fmtIter)
{
    fmtIter.finish();
}

// General version for C++98
#define TINYFORMAT_MAKE_FORMAT_DETAIL(n)                                  \
template<TINYFORMAT_ARGTYPES(n)>                                          \
void format(detail::FormatIterator& fmtIter, TINYFORMAT_VARARGS(n))       \
{                                                                         \
    fmtIter.accept(v1);                                                   \
    format(fmtIter TINYFORMAT_PASSARGS_TAIL(n));                          \
}

TINYFORMAT_FOREACH_ARGNUM(TINYFORMAT_MAKE_FORMAT_DETAIL)
#undef TINYFORMAT_MAKE_FORMAT_DETAIL

#endif // End C++98 variadic template emulation for format()

} // namespace detail


//------------------------------------------------------------------------------
// Implement all the main interface functions here in terms of detail::format()

#ifdef TINYFORMAT_USE_VARIADIC_TEMPLATES

// C++11 - the simple case
template<typename T1, typename... Args>
void format(std::ostream& out, const char* fmt, const T1& v1, const Args&... args)
{
    detail::FormatIterator fmtIter(out, fmt);
    format(fmtIter, v1, args...);
}

template<typename T1, typename... Args>
std::string format(const char* fmt, const T1& v1, const Args&... args)
{
    std::ostringstream oss;
    format(oss, fmt, v1, args...);
    return oss.str();
}

template<typename T1, typename... Args>
std::string format(const std::string &fmt, const T1& v1, const Args&... args)
{
    std::ostringstream oss;
    format(oss, fmt.c_str(), v1, args...);
    return oss.str();
}

template<typename T1, typename... Args>
void printf(const char* fmt, const T1& v1, const Args&... args)
{
    format(std::cout, fmt, v1, args...);
}

#else

// C++98 - define the interface functions using the wrapping macros
#define TINYFORMAT_MAKE_FORMAT_FUNCS(n)                                   \
                                                                          \
template<TINYFORMAT_ARGTYPES(n)>                                          \
void format(std::ostream& out, const char* fmt, TINYFORMAT_VARARGS(n))    \
{                                                                         \
    tinyformat::detail::FormatIterator fmtIter(out, fmt);                 \
    tinyformat::detail::format(fmtIter, TINYFORMAT_PASSARGS(n));          \
}                                                                         \
                                                                          \
template<TINYFORMAT_ARGTYPES(n)>                                          \
std::string format(const char* fmt, TINYFORMAT_VARARGS(n))                \
{                                                                         \
    std::ostringstream oss;                                               \
    tinyformat::format(oss, fmt, TINYFORMAT_PASSARGS(n));                 \
    return oss.str();                                                     \
}                                                                         \
                                                                          \
template<TINYFORMAT_ARGTYPES(n)>                                          \
std::string format(const std::string &fmt, TINYFORMAT_VARARGS(n))         \
{                                                                         \
    std::ostringstream oss;                                               \
    tinyformat::format(oss, fmt.c_str(), TINYFORMAT_PASSARGS(n));         \
    return oss.str();                                                     \
}                                                                         \
                                                                          \
template<TINYFORMAT_ARGTYPES(n)>                                          \
void printf(const char* fmt, TINYFORMAT_VARARGS(n))                       \
{                                                                         \
    tinyformat::format(std::cout, fmt, TINYFORMAT_PASSARGS(n));           \
}

TINYFORMAT_FOREACH_ARGNUM(TINYFORMAT_MAKE_FORMAT_FUNCS)
#undef TINYFORMAT_MAKE_FORMAT_FUNCS
#endif


//------------------------------------------------------------------------------
// Define deprecated wrapping macro for backward compatibility in tinyformat
// 1.x.  Will be removed in version 2!
#define TINYFORMAT_WRAP_FORMAT_EXTRA_ARGS
#define TINYFORMAT_WRAP_FORMAT_N(n, returnType, funcName, funcDeclSuffix,  \
                                 bodyPrefix, streamName, bodySuffix)       \
template<TINYFORMAT_ARGTYPES(n)>                                           \
returnType funcName(TINYFORMAT_WRAP_FORMAT_EXTRA_ARGS const char* fmt,     \
                    TINYFORMAT_VARARGS(n)) funcDeclSuffix                  \
{                                                                          \
    bodyPrefix                                                             \
    tinyformat::format(streamName, fmt, TINYFORMAT_PASSARGS(n));           \
    bodySuffix                                                             \
}                                                                          \

#define TINYFORMAT_WRAP_FORMAT(returnType, funcName, funcDeclSuffix,       \
                               bodyPrefix, streamName, bodySuffix)         \
inline                                                                     \
returnType funcName(TINYFORMAT_WRAP_FORMAT_EXTRA_ARGS const char* fmt      \
                    ) funcDeclSuffix                                       \
{                                                                          \
    bodyPrefix                                                             \
    tinyformat::detail::FormatIterator(streamName, fmt).finish();          \
    bodySuffix                                                             \
}                                                                          \
TINYFORMAT_WRAP_FORMAT_N(1 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(2 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(3 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(4 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(5 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(6 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(7 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(8 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(9 , returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(10, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(11, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(12, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(13, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(14, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(15, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \
TINYFORMAT_WRAP_FORMAT_N(16, returnType, funcName, funcDeclSuffix, bodyPrefix, streamName, bodySuffix) \


} // namespace tinyformat

#define strprintf tfm::format

#endif // TINYFORMAT_H_INCLUDED
