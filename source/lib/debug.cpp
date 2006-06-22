/**
 * =========================================================================
 * File        : debug.cpp
 * Project     : 0 A.D.
 * Description : platform-independent debug support code.
 *
 * @author Jan.Wassenberg@stud.uni-karlsruhe.de
 * =========================================================================
 */

/*
 * Copyright (c) 2005 Jan Wassenberg
 *
 * Redistribution and/or modification are also permitted under the
 * terms of the GNU General Public License as published by the
 * Free Software Foundation (version 2 or later, at your option).
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "precompiled.h"

#include <stdarg.h>
#include <string.h>

#include "lib.h"
#include "posix.h"
#include "lib/sysdep/cpu.h"	// CAS
// some functions here are called from within mmgr; disable its hooks
// so that our allocations don't cause infinite recursion.
#include "nommgr.h"
#include "app_hooks.h"
#include "lib/path_util.h"
#include "debug_stl.h"
#include "debug.h"


// needed when writing crashlog
static const size_t LOG_CHARS = 16384;
wchar_t debug_log[LOG_CHARS];
wchar_t* debug_log_pos = debug_log;

// write to memory buffer (fast)
void debug_wprintf_mem(const wchar_t* fmt, ...)
{
	const ssize_t chars_left = (ssize_t)LOG_CHARS - (debug_log_pos-debug_log);
	debug_assert(chars_left >= 0);

	// potentially not enough room for the new string; throw away the
	// older half of the log. we still protect against overflow below.
	if(chars_left < 512)
	{
		const size_t copy_size = sizeof(wchar_t) * LOG_CHARS/2;
		wchar_t* const middle = &debug_log[LOG_CHARS/2];
		memcpy2(debug_log, middle, copy_size);
		memset(middle, 0, copy_size);
		debug_log_pos -= LOG_CHARS/2;	// don't assign middle (may leave gap)
	}

	// write into buffer (in-place)
	va_list args;
	va_start(args, fmt);
	int len = vswprintf(debug_log_pos, chars_left-2, fmt, args);
	va_end(args);
	if(len < 0)
	{
		debug_warn("vswprintf failed");
		return;
	}
	debug_log_pos += len+2;
	wcscpy(debug_log_pos-2, L"\r\n");	// safe
}



// need to shoehorn printf-style variable params into
// the OutputDebugString call.
// - don't want to split into multiple calls - would add newlines to output.
// - fixing Win32 _vsnprintf to return # characters that would be written,
//   as required by C99, looks difficult and unnecessary. if any other code
//   needs that, implement GNU vasprintf.
// - fixed size buffers aren't nice, but much simpler than vasprintf-style
//   allocate+expand_until_it_fits. these calls are for quick debug output,
//   not loads of data, anyway.

// max # characters (including \0) output by debug_(w)printf in one call.
static const int MAX_CHARS = 512;


// rationale: static data instead of std::set to allow setting at any time.
// we store FNV hash of tag strings for fast comparison; collisions are
// extremely unlikely and can only result in displaying more/less text.
static const uint MAX_TAGS = 20;
static u32 tags[MAX_TAGS];
static uint num_tags;

void debug_filter_add(const char* tag)
{
	const u32 hash = fnv_hash(tag);

	// make sure it isn't already in the list
	for(uint i = 0; i < MAX_TAGS; i++)
		if(tags[i] == hash)
			return;

	// too many already?
	if(num_tags == MAX_TAGS)
	{
		debug_warn("increase MAX_TAGS");
		return;
	}

	tags[num_tags++] = hash;
}

void debug_filter_remove(const char* tag)
{
	const u32 hash = fnv_hash(tag);

	for(uint i = 0; i < MAX_TAGS; i++)
		// found it
		if(tags[i] == hash)
		{
			// replace with last element (avoid holes)
			tags[i] = tags[MAX_TAGS-1];
			num_tags--;

			// can only happen once, so we're done.
			return;
		}
}

void debug_filter_clear()
{
	for(uint i = 0; i < MAX_TAGS; i++)
		tags[i] = 0;
}

static bool filter_allows(const char* text)
{
	uint i;
	for(i = 0; ; i++)
	{
		// no | found => no tag => should always be displayed
		if(text[i] == ' ' || text[i] == '\0')
			return true;
		if(text[i] == '|' && i != 0)
			break;
	}

	const u32 hash = fnv_hash(text, i);

	// check if entry allowing this tag is found
	for(i = 0; i < MAX_TAGS; i++)
		if(tags[i] == hash)
			return true;

	return false;
}


void debug_printf(const char* fmt, ...)
{
	char buf[MAX_CHARS]; buf[ARRAY_SIZE(buf)-1] = '\0';

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, MAX_CHARS-1, fmt, ap);
	va_end(ap);

	if(filter_allows(buf))
		debug_puts(buf);
}

void debug_wprintf(const wchar_t* fmt, ...)
{
	wchar_t wcs_buf[MAX_CHARS]; wcs_buf[ARRAY_SIZE(wcs_buf)-1] = '\0';

	va_list ap;
	va_start(ap, fmt);
	vswprintf(wcs_buf, MAX_CHARS-1, fmt, ap);
	va_end(ap);

	// convert wchar_t to UTF-8.
	//
	// rationale: according to fwide(3) and assorted manpage, FILEs are in
	// single character or in wide character mode. When a FILE is in
	// single character mode, wide character writes will fail, and no
	// conversion is done automatically. Thus the manual conversion.
	//
	// it's done here (instead of in OS-specific debug_putws) because
	// filter_allow requires the conversion also.
	//
	// jw: MSDN wcstombs dox say 2 bytes per wchar is enough.
	// not sure about this; to be on the safe side, we check for overflow.
	const size_t MAX_BYTES = MAX_CHARS*2;
	char mbs_buf[MAX_BYTES]; mbs_buf[MAX_BYTES-1] = '\0';
	size_t bytes_written = wcstombs(mbs_buf, wcs_buf, MAX_BYTES);
	// .. error
	if(bytes_written == (size_t)-1)
		debug_warn("invalid wcs character encountered");
	// .. exact fit, make sure it's 0-terminated
	if(bytes_written == MAX_BYTES)
		mbs_buf[MAX_BYTES-1] = '\0';
	// .. paranoia: overflow is impossible
	debug_assert(bytes_written <= MAX_BYTES);

	if(filter_allows(mbs_buf))
		debug_puts(mbs_buf);
}


//-----------------------------------------------------------------------------

LibError debug_write_crashlog(const wchar_t* text)
{
	// note: we go through some gyrations here (strcpy+strcat) to avoid
	// dependency on file code (path_append).
	char N_path[PATH_MAX];
	strcpy_s(N_path, ARRAY_SIZE(N_path), ah_get_log_dir());
	strcat_s(N_path, ARRAY_SIZE(N_path), "crashlog.txt");
	FILE* f = fopen(N_path, "w");
	if(!f)
		WARN_RETURN(ERR_FILE_ACCESS);

	fputwc(0xfeff, f);	// BOM
	fwprintf(f, L"%ls\n", text);
	fwprintf(f, L"\n\n====================================\n\n");

	// allow user to bundle whatever information they want
	ah_bundle_logs(f);

	fwprintf(f, L"Last known activity:\n\n %ls\n", debug_log);

	fclose(f);
	return INFO_OK;
}



//////////////////////////////////////////////////////////////////////////////
//
// storage for and construction of strings describing a symbol
//
//////////////////////////////////////////////////////////////////////////////

// tightly pack strings within one large buffer. we never need to free them,
// since the program structure / addresses can never change.
static const size_t STRING_BUF_SIZE = 64*KiB;
static char* string_buf;
static char* string_buf_pos;

static const char* symbol_string_build(void* symbol, const char* name, const char* file, int line)
{
	// maximum bytes allowed per string (arbitrary).
	// needed to prevent possible overflows.
	const size_t STRING_MAX = 1000;

	if(!string_buf)
	{
		string_buf = (char*)malloc(STRING_BUF_SIZE);
		if(!string_buf)
		{
			WARN_ERR(ERR_NO_MEM);
			return 0;
		}
		string_buf_pos = string_buf;
	}

	// make sure there's enough space for a new string
	char* string = string_buf_pos;
	if(string + STRING_MAX >= string_buf + STRING_BUF_SIZE)
	{
		WARN_ERR(ERR_LIMIT);
		return 0;
	}

	// user didn't know name/file/line. attempt to resolve from debug info.
	char name_buf[DBG_SYMBOL_LEN];
	char file_buf[DBG_FILE_LEN];
	if(!name || !file || !line)
	{
		int line_buf;
		(void)debug_resolve_symbol(symbol, name_buf, file_buf, &line_buf);

		// only override the original parameters if value is meaningful;
		// otherwise, stick with what we got, even if 0.
		// (obviates test of return value; correctly handles partial failure).
		if(name_buf[0])
			name = name_buf;
		if(file_buf[0])
			file = file_buf;
		if(line_buf)
			line = line_buf;
	}

	// file and line are available: write them
	int len;
	if(file && line)
	{
		// strip path from filename (long and irrelevant)
		const char* fn_only = path_name_only(file);

		len = snprintf(string, STRING_MAX-1, "%s:%05d ", fn_only, line);
	}
	// only address is known
	else
		len = snprintf(string, STRING_MAX-1, "%p ", symbol);

	// append symbol name
	if(name)
	{
		snprintf(string+len, STRING_MAX-1-len, "%s", name);
		debug_stl_simplify_name(string+len);
	}

	return string;
}


//////////////////////////////////////////////////////////////////////////////
//
// cache, mapping symbol address to its description string.
//
//////////////////////////////////////////////////////////////////////////////

// note: we don't want to allocate a new string for every symbol -
// that would waste lots of memory. instead, when a new address is first
// encountered, allocate a string describing it, and store for later use.

// hash table entry; valid iff symbol != 0. the string pointer must remain
// valid until the cache is shut down.
struct Symbol
{
	void* symbol;
	const char* string;
};

static const uint MAX_SYMBOLS = 2048;
static Symbol* symbols;
static uint total_symbols;


static uint hash_jumps;

// strip off lower 2 bits, since it's unlikely that 2 symbols are
// within 4 bytes of one another.
static uint hash(void* symbol)
{
	const uintptr_t address = (uintptr_t)symbol;
	return (uint)( (address >> 2) % MAX_SYMBOLS );
}


// algorithm: hash lookup with linear probing.
static const char* symbol_string_from_cache(void* symbol)
{
	// hash table not initialized yet, nothing to find
	if(!symbols)
		return 0;

	uint idx = hash(symbol);
	for(;;)
	{
		Symbol* c = &symbols[idx];

		// not in table
		if(!c->symbol)
			return 0;
		// found
		if(c->symbol == symbol)
			return c->string;

		idx = (idx+1) % MAX_SYMBOLS;
	}
}


// associate <string> (must remain valid) with <symbol>, for
// later calls to symbol_string_from_cache.
static void symbol_string_add_to_cache(const char* string, void* symbol)
{
	if(!symbols)
	{
		// note: must be zeroed to set each Symbol to "invalid"
		symbols = (Symbol*)calloc(MAX_SYMBOLS, sizeof(Symbol));
		if(!symbols)
			debug_warn("failed to allocate symbols");
	}

	// hash table is completely full (guard against infinite loop below).
	// if this happens, the string won't be cached - nothing serious.
	if(total_symbols >= MAX_SYMBOLS)
		WARN_ERR_RETURN(ERR_LIMIT);
	total_symbols++;

	// find Symbol slot in hash table
	Symbol* c;
	uint idx = hash(symbol);
	for(;;)
	{
		c = &symbols[idx];

		// found an empty slot
		if(!c->symbol)
			break;

		idx = (idx+1) % MAX_SYMBOLS;
		hash_jumps++;
	}

	// commit Symbol information
	c->symbol  = symbol;
	c->string = string;

	string_buf_pos += strlen(string)+1;
}




const char* debug_get_symbol_string(void* symbol, const char* name, const char* file, int line)
{
	// return it if already in cache
	const char* string = symbol_string_from_cache(symbol);
	if(string)
		return string;

	// try to build a new string
	string = symbol_string_build(symbol, name, file, line);
	if(!string)
		return 0;

	symbol_string_add_to_cache(string, symbol);

	return string;
}


//-----------------------------------------------------------------------------
// output
//-----------------------------------------------------------------------------

// translates and displays the given strings in a dialog.
// this is typically only used when debug_display_error has failed or
// is unavailable because that function is much more capable.
// implemented via sys_display_msgw; see documentation there.
void debug_display_msgw(const wchar_t* caption, const wchar_t* msg)
{
	sys_display_msgw(ah_translate(caption), ah_translate(msg));
}


// when an error has come up and user clicks Exit, we don't want any further
// errors (e.g. caused by atexit handlers) to come up, possibly causing an
// infinite loop. it sucks to hide errors, but we assume that whoever clicked
// exit really doesn't want to see any more errors.
static bool exit_requested;

// this logic is applicable to any type of error. special cases such as
// suppressing certain expected WARN_ERRs are done there.
static bool should_suppress_error(u8* suppress)
{
	if(!suppress)
		return false;

	if(*suppress == DEBUG_SUPPRESS)
		return true;

	if(exit_requested)
		return true;

	return false;
}

static const wchar_t* build_error_message(wchar_t* buf, size_t max_chars,
	const wchar_t* description,
	const char* fn_only, int line, const char* func,
	uint skip, void* context,
	bool is_nested_error)
{
	if(!buf)
		return L"(insufficient memory to generate error message)";

	static const wchar_t fmt[] =
		L"%ls\r\n"
		L"Location: %hs:%d (%hs)\r\n"
		L"\r\n"
		L"Call stack:\r\n"
		L"\r\n";
	int len = swprintf(buf,max_chars,fmt, description, fn_only, line, func);
	if(len < 0)
		return L"(error while formatting error message)";

	// add stack trace to end of message
	wchar_t* pos = buf+len; const size_t chars_left = max_chars-len;
	if(!is_nested_error)
	{
		if(!context)
			skip++;	// skip this frame
		debug_dump_stack(pos, chars_left, skip, context);
	}
	// .. except when a stack trace is currently already in progress
	//    (debug_dump_stack is not reentrant due to use of global buffer!)
	else
	{
		wcscpy_s(pos, chars_left,
			L"(cannot start a nested stack trace; what probably happened is that "
			L"an debug_assert/debug_warn/CHECK_ERR fired during the current trace.)"
		);
	}

	return buf;
}

static ErrorReaction call_display_error(const wchar_t* text, uint flags)
{
	// first try app hook implementation
	ErrorReaction er = ah_display_error(text, flags);
	// .. it's only a stub: default to normal implementation
	if(er == ER_NOT_IMPLEMENTED)
		er = sys_display_error(text, flags);

	return er;
}

static ErrorReaction carry_out_ErrorReaction(ErrorReaction er, uint flags, u8* suppress)
{
	const bool manual_break = (flags & DE_MANUAL_BREAK) != 0;

	switch(er)
	{
	case ER_BREAK:
		// handle "break" request unless the caller wants to (doing so here
		// instead of within the dlgproc yields a correct call stack)
		if(!manual_break)
		{
			debug_break();
			er = ER_CONTINUE;
		}
		break;

	case ER_SUPPRESS:
		*suppress = DEBUG_SUPPRESS;
		er = ER_CONTINUE;
		break;

	case ER_EXIT:
		exit_requested = true;	// see declaration

		// disable memory-leak reporting to avoid a flood of warnings
		// (lots of stuff will leak since we exit abnormally).
		debug_heap_enable(DEBUG_HEAP_NONE);
#if CONFIG_USE_MMGR
		mmgr_set_options(0);
#endif

		exit(EXIT_FAILURE);
	}

	return er;
}

ErrorReaction debug_display_error(const wchar_t* description,
	uint flags, uint skip, void* context,
	const char* file, int line, const char* func,
	u8* suppress)
{
	// "suppressing" this error means doing nothing and returning ER_CONTINUE.
	if(should_suppress_error(suppress))
		return ER_CONTINUE;

	// fix up params
	// .. translate
	description = ah_translate(description);
	// .. caller supports a suppress flag; set the corresponding flag so that
	//    the error display implementation enables the Suppress option.
	if(suppress)
		flags |= DE_ALLOW_SUPPRESS;
	// .. deal with incomplete file/line info
	if(!file || file[0] == '\0')
		file = "unknown";
	if(line <= 0)
		line = 0;
	if(!func || func[0] == '\0')
		func = "?";
	// .. _FILE__ evaluates to the full path (albeit without drive letter)
	//    which is rather long. we only display the base name for clarity.
	const char* fn_only = path_name_only(file);


	// display in output window; double-click will navigate to error location.
	debug_wprintf(L"%hs(%d): %ls\n", fn_only, line, description);


	// allocate memory for the error message. this needs to be quite large,
	// so preallocating is undesirable. 
	// note: this code can't be moved into a subroutine due to alloca.
	wchar_t* buf = 0;
	size_t max_chars = 256*KiB;
	// .. try allocating from heap. can't rely on this because we might
	//    be called upon to report heap corruption errors.
	void* heap_mem = malloc((max_chars+1)*sizeof(wchar_t));
	buf = (wchar_t*)heap_mem;
	// .. heap alloc failed; try allocating from stack. if this fails,
	//    we give up and simply display a static error message.
	if(!buf)
	{
		max_chars = 128*KiB;	// (stack limit is usually 1 MiB)
		buf = (wchar_t*)alloca((max_chars+1)*sizeof(wchar_t));
	}

	static uintptr_t already_in_progress;
	const bool is_nested = !CAS(&already_in_progress, 0, 1);

	const wchar_t* text = build_error_message(buf, max_chars, description,
		fn_only, line, func, skip, context, is_nested);

	if(!is_nested)	// avoids potential infinite loop
		debug_write_crashlog(text);

	ErrorReaction er = call_display_error(text, flags);


	// note: debug_break-ing here to make sure the app doesn't continue
	// running is no longer necessary. debug_display_error now determines our
	// window handle and is modal.

	// must happen before carry_out_ErrorReaction because that may exit.
	// note: no-op if not allocated from heap.
	free(heap_mem);

	already_in_progress = 0;

	return carry_out_ErrorReaction(er, flags, suppress);
}




ErrorReaction debug_assert_failed(const char* expr, u8* suppress,
	const char* file, int line, const char* func)
{
	uint skip = 1; void* context = 0;
	wchar_t buf[400];
	swprintf(buf, ARRAY_SIZE(buf), L"Assertion failed: \"%hs\"", expr);
	return debug_display_error(buf, DE_MANUAL_BREAK, skip,context, file,line,func, suppress);
}


ErrorReaction debug_warn_err(LibError err, u8* suppress,
	const char* file, int line, const char* func)
{
	// for edge cases in some functions, warnings (=asserts) are raised in
	// addition to returning an error code. self-tests deliberately trigger
	// these cases and check for the latter but shouldn't cause the former.
	// we therefore squelch them here.
	//TODO squelch certain errors once

	uint skip = 1; void* context = 0;
	wchar_t buf[400];
	char err_buf[200]; error_description_r(err, err_buf, ARRAY_SIZE(err_buf));
	swprintf(buf, ARRAY_SIZE(buf), L"Function call failed: return value was %d (%hs)", err, err_buf);
	return debug_display_error(buf, DE_MANUAL_BREAK, skip,context, file,line,func, suppress);
}


//-----------------------------------------------------------------------------
// thread naming
//-----------------------------------------------------------------------------

// when debugging multithreading problems, logging the currently running
// thread is helpful; a user-specified name is easier to remember than just
// the thread handle. to that end, we provide a robust TLS mechanism that is
// much safer than the previous method of hijacking TIB.pvArbitrary.
//
// note: on Win9x thread "IDs" are pointers to the TIB xor-ed with an
// obfuscation value calculated at boot-time.
//
// __declspec(thread) et al. are now available on VC and newer GCC but we
// implement TLS manually (via pthread_setspecific) to ensure compatibility.

static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;


// provided for completeness and to avoid displaying bogus resource leaks.
static void tls_shutdown()
{
	WARN_ERR(pthread_key_delete(tls_key));
	tls_key = 0;
}


// (called via pthread_once from debug_set_thread_name)
static void tls_init()
{
	WARN_ERR(pthread_key_create(&tls_key, 0));	// no dtor

	// note: do not use atexit; this may be called before _cinit.
}


// set the current thread's name; it will be returned by subsequent calls to
// debug_get_thread_name.
//
// the string pointed to by <name> MUST remain valid throughout the
// entire program; best to pass a string literal. allocating a copy
// would be quite a bit more work due to cleanup issues.
//
// if supported on this platform, the debugger is notified of the new name;
// it will be displayed there instead of just the handle.
void debug_set_thread_name(const char* name)
{
	WARN_ERR(pthread_once(&tls_once, tls_init));

	WARN_ERR(pthread_setspecific(tls_key, name));

#if OS_WIN
	wdbg_set_thread_name(name);
#endif
}


// return the pointer assigned by debug_set_thread_name or 0 if
// that hasn't been done yet for this thread.
const char* debug_get_thread_name()
{
	return (const char*)pthread_getspecific(tls_key);
}




void debug_shutdown()
{
	tls_shutdown();
}
