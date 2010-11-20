/* Copyright (c) 2010 Wildfire Games
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * platform-independent debug support code.
 */

#ifndef INCLUDED_DEBUG
#define INCLUDED_DEBUG

// this module provides platform-independent debug facilities, useful for
// diagnosing and reporting program errors.
// - a symbol engine provides access to compiler-generated debug information and
//   can also give a stack trace including local variables;
// - our more powerful assert() replacement gives a stack trace so
//   that the underlying problem becomes apparent;
// - the output routines make for platform-independent logging and
//   crashlogs with "last-known activity" reporting.

#include "lib/lib_errors.h"
#include "lib/code_generation.h"

/**
 * trigger a breakpoint when reached/"called".
 * if defined as a macro, the debugger can break directly into the
 * target function instead of one frame below it as with a conventional
 * call-based implementation.
 **/
#if MSC_VERSION
# define debug_break __debugbreak	// intrinsic "function"
#else
extern void debug_break();
#endif

// for widening non-literals (e.g. __FILE__)
// note: C99 says __func__ is a magic *variable*, and GCC doesn't allow
// widening it via preprocessor.
#define WIDEN2(x) L ## x
#define WIDEN(x) WIDEN2(x)


//-----------------------------------------------------------------------------
// output
//-----------------------------------------------------------------------------

/**
 * write a formatted string to the debug channel, subject to filtering
 * (see below). implemented via debug_puts - see performance note there.
 *
 * @param fmt Format string and varargs; see printf.
 **/
LIB_API void debug_printf(const wchar_t* fmt, ...) WPRINTF_ARGS(1);


/**
 * translates and displays the given strings in a dialog.
 * this is typically only used when debug_DisplayError has failed or
 * is unavailable because that function is much more capable.
 * implemented via sys_display_msg; see documentation there.
 **/
LIB_API void debug_DisplayMessage(const wchar_t* caption, const wchar_t* msg);

/// flags to customize debug_DisplayError behavior
enum DebugDisplayErrorFlags
{
	/**
	 * disallow the Continue button. used e.g. if an exception is fatal.
	 **/
	DE_NO_CONTINUE = 1,

	/**
	 * enable the Suppress button. set automatically by debug_DisplayError if
	 * it receives a non-NULL suppress pointer. a flag is necessary because
	 * the sys_display_error interface doesn't get that pointer.
	 * rationale for automatic setting: this may prevent someone from
	 * forgetting to specify it, and disabling Suppress despite having
	 * passed a non-NULL pointer doesn't make much sense.
	 **/
	DE_ALLOW_SUPPRESS = 2,

	/**
	 * do not trigger a breakpoint inside debug_DisplayError; caller
	 * will take care of this if ER_BREAK is returned. this is so that the
	 * debugger can jump directly into the offending function.
	 **/
	DE_MANUAL_BREAK = 4,

	/**
	 * display just the given message; do not add any information about the
	 * call stack, do not write crashlogs, etc.
	 */
	DE_NO_DEBUG_INFO = 8
};

/**
 * a bool that is reasonably certain to be set atomically.
 * we cannot assume support for OpenMP (requires GCC 4.2) or C++0x,
 * so we'll have to resort to intptr_t, cpu_CAS and COMPILER_FENCE.
 **/
typedef volatile intptr_t atomic_bool;

/**
 * value for suppress flag once set by debug_DisplayError.
 * rationale: this value is fairly distinctive and helps when
 * debugging the symbol engine.
 * initial value is 0 rather that another constant; this avoids
 * allocating .rdata space.
 **/
const atomic_bool DEBUG_SUPPRESS = 0xAB;

/**
 * choices offered by the shared error dialog
 **/
enum ErrorReaction
{
	/**
	 * ignore, continue as if nothing happened.
	 * note: value doesn't start at 0 because that is interpreted as a
	 * DialogBoxParam failure.
	 **/
	ER_CONTINUE = 1,

	/**
	 * trigger breakpoint, i.e. enter debugger.
	 * only returned if DE_MANUAL_BREAK was passed; otherwise,
	 * debug_DisplayError will trigger a breakpoint itself.
	 **/
	ER_BREAK,

	/**
	 * ignore and do not report again.
	 * note: non-persistent; only applicable during this program run.
	 * acted on by debug_DisplayError; never returned to caller.
	 **/
	ER_SUPPRESS,

	/**
	 * exit the program immediately.
	 * acted on by debug_DisplayError; never returned to caller.
	 **/
	ER_EXIT,

	/**
	 * special return value for the display_error app hook stub to indicate
	 * that it has done nothing and that the normal sys_display_error
	 * implementation should be called instead.
	 * acted on by debug_DisplayError; never returned to caller.
	 **/
	ER_NOT_IMPLEMENTED
};

/**
 * display an error dialog with a message and stack trace.
 *
 * @param description text to show.
 * @param flags: see DebugDisplayErrorFlags.
 * @param context, lastFuncToSkip: see debug_DumpStack.
 * @param file, line, func: location of the error (typically passed as
 * WIDEN(__FILE__), __LINE__, __func__ from a macro)
 * @param suppress pointer to a caller-allocated flag that can be used to
 * suppress this error. if NULL, this functionality is skipped and the
 * "Suppress" dialog button will be disabled.
 * note: this flag is read and written exclusively here; caller only
 * provides the storage. values: see DEBUG_SUPPRESS above.
 * @return ErrorReaction (user's choice: continue running or stop?)
 **/
LIB_API ErrorReaction debug_DisplayError(const wchar_t* description, size_t flags, void* context, const wchar_t* lastFuncToSkip, const wchar_t* file, int line, const char* func, atomic_bool* suppress);

/**
 * convenience version, in case the advanced parameters aren't needed.
 * macro instead of providing overload/default values for C compatibility.
 **/
#define DEBUG_DISPLAY_ERROR(text) debug_DisplayError(text, 0, 0, L"debug_DisplayError", WIDEN(__FILE__),__LINE__,__func__, 0)


//
// filtering
//

/**
 * debug output is very useful, but "too much of a good thing can kill you".
 * we don't want to require different LOGn() macros that are enabled
 * depending on "debug level", because changing that entails lengthy
 * compiles and it's too coarse-grained. instead, we require all
 * strings to start with "tag_string|" (exact case and no quotes;
 * the alphanumeric-only \<tag_string\> identifies output type).
 * they are then subject to filtering: only if the tag has been
 * "added" via debug_filter_add is the appendant string displayed.
 *
 * this approach is easiest to implement and is fine because we control
 * all logging code. LIMODS falls from consideration since it's not
 * portable and too complex.
 *
 * notes:
 * - filter changes only affect subsequent debug_*printf calls;
 *   output that didn't pass the filter is permanently discarded.
 * - strings not starting with a tag are always displayed.
 * - debug_filter_* can be called at any time and from the debugger,
 *   but are not reentrant.
 *
 * in future, allow output with the given tag to proceed.
 * no effect if already added.
 **/
LIB_API void debug_filter_add(const wchar_t* tag);

/**
 * in future, discard output with the given tag.
 * no effect if not currently added.
 **/
LIB_API void debug_filter_remove(const wchar_t* tag);

/**
 * clear all filter state; equivalent to debug_filter_remove for
 * each tag that was debug_filter_add-ed.
 **/
LIB_API void debug_filter_clear();

/**
 * indicate if the given text would be printed.
 * useful for a series of debug_printfs - avoids needing to add a tag to
 * each of their format strings.
 **/
LIB_API bool debug_filter_allows(const wchar_t* text);

/**
 * write an error description and all logs into crashlog.txt
 * (in unicode format).
 *
 * @param text description of the error (including stack trace);
 * typically generated by debug_BuildErrorMessage.
 *
 * @return LibError; ERR::REENTERED if reentered via recursion or
 * multithreading (not allowed since an infinite loop may result).
 **/
LIB_API LibError debug_WriteCrashlog(const wchar_t* text);


//-----------------------------------------------------------------------------
// debug_assert
//-----------------------------------------------------------------------------

/**
 * make sure the expression \<expr\> evaluates to non-zero. used to validate
 * invariants in the program during development and thus gives a
 * very helpful warning if something isn't going as expected.
 * sprinkle these liberally throughout your code!
 *
 * recommended use is debug_assert(expression && "descriptive string") -
 * the string can pass more information about the problem on to whomever
 * is seeing the error.
 *
 * rationale: we call this "debug_assert" instead of "assert" for the
 * following reasons:
 * - consistency (everything here is prefixed with debug_) and
 * - to avoid inadvertent use of the much less helpful built-in CRT assert.
 *   if we were to override assert, it would be difficult to tell whether
 *   user source has included \<assert.h\> (possibly indirectly via other
 *   headers) and thereby stomped on our definition.
 **/
#define debug_assert(expr) \
STMT(\
	static atomic_bool suppress__;\
	if(!(expr))\
	{\
	switch(debug_OnAssertionFailure(WIDEN(#expr), &suppress__, WIDEN(__FILE__), __LINE__, __func__))\
		{\
		case ER_BREAK:\
			debug_break();\
			break;\
		default:\
			break;\
		}\
	}\
)

/**
 * show a dialog to make sure unexpected states in the program are noticed.
 * this is less error-prone than "debug_assert(0 && "text");" and avoids
 * "conditional expression is constant" warnings. we'd really like to
 * completely eliminate the problem; replacing 0 literals with LIB_API
 * volatile variables fools VC7 but isn't guaranteed to be free of overhead.
 * we therefore just squelch the warning (unfortunately non-portable).
 * this duplicates the code from debug_assert to avoid compiler warnings about
 * constant conditions.
 *
 * if being able to suppress the warning is desirable (e.g. for self-tests),
 * then use DEBUG_WARN_ERR instead.
 **/
#define debug_warn(expr) \
STMT(\
	static atomic_bool suppress__;\
	switch(debug_OnAssertionFailure(expr, &suppress__, WIDEN(__FILE__), __LINE__, __func__))\
	{\
	case ER_BREAK:\
		debug_break();\
		break;\
	default:\
		break;\
	}\
)


/**
 * if (LibError)err indicates an function failed, display the error dialog.
 * used by CHECK_ERR et al., which wrap function calls and automatically
 * warn user and return to caller.
 **/
#define DEBUG_WARN_ERR(err)\
STMT(\
	static atomic_bool suppress__;\
	switch(debug_OnError(err, &suppress__, WIDEN(__FILE__), __LINE__, __func__))\
	{\
	case ER_BREAK:\
		debug_break();\
		break;\
	default:\
		break;\
	}\
)


/**
 * called when a debug_assert fails;
 * notifies the user via debug_DisplayError.
 *
 * @param assert_expr the expression that failed; typically passed as
 * \#expr in the assert macro.
 * @param suppress see debug_DisplayError.
 * @param file, line source file name and line number of the spot that failed
 * @param func name of the function containing it
 * @return ErrorReaction (user's choice: continue running or stop?)
 **/
LIB_API ErrorReaction debug_OnAssertionFailure(const wchar_t* assert_expr, atomic_bool* suppress, const wchar_t* file, int line, const char* func);

/**
 * called when a DEBUG_WARN_ERR indicates an error occurred;
 * notifies the user via debug_DisplayError.
 *
 * @param err LibError value indicating the error that occurred
 * @param suppress see debug_DisplayError.
 * @param file, line source file name and line number of the spot that failed
 * @param func name of the function containing it
 * @return ErrorReaction (user's choice: continue running or stop?)
 **/
LIB_API ErrorReaction debug_OnError(LibError err, atomic_bool* suppress, const wchar_t* file, int line, const char* func);


/**
 * suppress (prevent from showing) the error dialog from subsequent
 * debug_OnError for the given LibError.
 *
 * rationale: for edge cases in some functions, warnings are raised in
 * addition to returning an error code. self-tests deliberately trigger
 * these cases and check for the latter but shouldn't cause the former.
 * we therefore need to squelch them.
 *
 * @param err the LibError to skip.
 *
 * note: only one concurrent skip request is allowed; call
 * debug_StopSkippingErrors before the next debug_SkipErrors.
 */
LIB_API void debug_SkipErrors(LibError err);

LIB_API size_t debug_StopSkippingErrors();


//-----------------------------------------------------------------------------
// symbol access
//-----------------------------------------------------------------------------

namespace ERR
{
	const LibError SYM_NO_STACK_FRAMES_FOUND = -100400;
	const LibError SYM_UNRETRIEVABLE_STATIC  = -100401;
	const LibError SYM_UNRETRIEVABLE         = -100402;
	const LibError SYM_TYPE_INFO_UNAVAILABLE = -100403;
	const LibError SYM_INTERNAL_ERROR        = -100404;
	const LibError SYM_UNSUPPORTED           = -100405;
	const LibError SYM_CHILD_NOT_FOUND       = -100406;
	// this limit is to prevent infinite recursion.
	const LibError SYM_NESTING_LIMIT         = -100407;
	// this limit is to prevent large symbols (e.g. arrays or linked lists)
	// from taking up all available output space.
	const LibError SYM_SINGLE_SYMBOL_LIMIT   = -100408;
}

namespace INFO
{
	// one of the dump_sym* functions decided not to output anything at
	// all (e.g. for member functions in UDTs - we don't want those).
	// therefore, skip any post-symbol formatting (e.g. ) as well.
	const LibError SYM_SUPPRESS_OUTPUT       = +100409;
}


/**
 * Maximum number of characters (including trailing \\0) written to
 * user's buffers by debug_ResolveSymbol.
 **/
const size_t DBG_SYMBOL_LEN = 1000;
const size_t DBG_FILE_LEN = 100;

/**
 * read and return symbol information for the given address.
 *
 * NOTE: the PDB implementation is rather slow (~500us).
 *
 * @param ptr_of_interest address of symbol (e.g. function, variable)
 * @param sym_name optional out; size >= DBG_SYMBOL_LEN chars;
 * receives symbol name returned via debug info.
 * @param file optional out; size >= DBG_FILE_LEN chars; receives
 * base name only (no path; see rationale in wdbg_sym) of
 * source file containing the symbol.
 * @param line optional out; receives source file line number of symbol.
 *
 * note: all of the output parameters are optional; we pass back as much
 * information as is available and desired.
 * @return LibError; INFO::OK iff any information was successfully
 * retrieved and stored.
 **/
LIB_API LibError debug_ResolveSymbol(void* ptr_of_interest, wchar_t* sym_name, wchar_t* file, int* line);

/**
 * write a complete stack trace (including values of local variables) into
 * the specified buffer.
 *
 * @param buf Target buffer.
 * @param maxChars Max chars of buffer (should be several thousand).
 * @param context Platform-specific representation of execution state
 *		  (e.g. Win32 CONTEXT). if not NULL, tracing starts there; this is useful
 *		  for exceptions. Otherwise, tracing starts from the current call stack.
 * @param lastFuncToSkip Is used for omitting error-reporting functions like
 *		  debug_OnAssertionFailure from the stack trace. It is either 0 (skip nothing) or
 *		  a substring of a function's name (this allows platform-independent
 *		  matching of stdcall-decorated names).
 *		  Rationale: this is safer than specifying a fixed number of frames,
 *		  which can be incorrect due to inlining.
 * @return LibError; ERR::REENTERED if reentered via recursion or
 *		   multithreading (not allowed since static data is used).
 **/
LIB_API LibError debug_DumpStack(wchar_t* buf, size_t maxChars, void* context, const wchar_t* lastFuncToSkip);


//-----------------------------------------------------------------------------
// helper functions (used by implementation)
//-----------------------------------------------------------------------------

/**
 * [system-dependent] write a string to the debug channel.
 * this can be quite slow (~1 ms)! On Windows, it uses OutputDebugString
 * (entails context switch), otherwise stdout+fflush (waits for IO).
 **/
LIB_API void debug_puts(const wchar_t* text);

/**
 * return the caller of a certain function on the call stack.
 *
 * this function is useful for recording (partial) stack traces for
 * memory allocation tracking, etc.
 *
 * @param context, lastFuncToSkip - see debug_DumpStack
 * @return address of the caller
 **/
LIB_API void* debug_GetCaller(void* context, const wchar_t* lastFuncToSkip);

/**
 * check if a pointer appears to be totally invalid.
 *
 * this check is not authoritative (the pointer may be "valid" but incorrect)
 * but can be used to filter out obviously wrong values in a portable manner.
 *
 * @param p pointer
 * @return 1 if totally bogus, otherwise 0.
 **/
LIB_API int debug_IsPointerBogus(const void* p);

/// does the given pointer appear to point to code?
LIB_API bool debug_IsCodePointer(void* p);

/// does the given pointer appear to point to the stack?
LIB_API bool debug_IsStackPointer(void* p);


/**
 * inform the debugger of the current thread's name.
 *
 * (threads are easier to keep apart when they are identified by
 * name rather than TID.)
 **/
LIB_API void debug_SetThreadName(const char* name);


/**
 * holds memory for an error message.
 **/
struct ErrorMessageMem
{
	// rationale:
	// - error messages with stack traces require a good deal of memory
	//   (hundreds of KB). static buffers of that size are undesirable.
 	// - the heap may be corrupted, so don't use malloc. allocator.h's
	//   page_aligned_malloc (implemented via mmap) should be safe.
	// - alloca is a bit iffy (the stack may be maxed out), non-portable and
	//   complicates the code because it can't be allocated by a subroutine.
	// - this method is probably slow, but error messages aren't built often.
	//   if necessary, first try malloc and use mmap if that fails.
	void* pa_mem;
};

/**
 * free memory from the error message.
 *
 * @param emm ErrorMessageMem*
 **/
LIB_API void debug_FreeErrorMessage(ErrorMessageMem* emm);

/**
 * build a string describing the given error.
 *
 * this is a helper function used by debug_DumpStack and is made available
 * so that the self-test doesn't have to display the error dialog.
 *
 * @param description: general description of the problem.
 * @param fn_only filename (no path) of source file that triggered the error.
 * @param line, func: exact position of the error.
 * @param context, lastFuncToSkip: see debug_DumpStack.
 * @param emm memory for the error message. caller should allocate
 * stack memory and set alloc_buf*; if not, there will be no
 * fallback in case heap alloc fails. should be freed via
 * debug_FreeErrorMessage when no longer needed.
 **/
LIB_API const wchar_t* debug_BuildErrorMessage(const wchar_t* description, const wchar_t* fn_only, int line, const char* func, void* context, const wchar_t* lastFuncToSkip, ErrorMessageMem* emm);

#endif	// #ifndef INCLUDED_DEBUG
