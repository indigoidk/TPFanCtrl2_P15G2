#include "_prec.h"

/// bounded strlen that first forces a NUL terminator into the final slot.
/// Takes a WRITABLE buffer: it mutates str[SourceLen-1], so the parameter must
/// be non-const (the old const char* + const_cast signature lied about that and
/// was undefined behavior if ever handed a literal / read-only buffer).
inline size_t strlen_s(char * str, const size_t SourceLen) {
    str[SourceLen-1] = 0;
    return strnlen(str, SourceLen);
}