/* New file added by Cataclysm: The Last Generation. */

// MSYS2 MSYS (which uses CYGWIN) does not seem to provide locale independent
// string conversion functions, but it also seems to only support the C locale,
// so might as well allow using the locale dependent functions.
#if !defined(__CYGWIN__) || defined(_WIN32)
  #error CTLG requires locale independent number parsing.
#endif
