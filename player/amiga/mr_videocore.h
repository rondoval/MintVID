#ifndef MR_VIDEOCORE_H
#define MR_VIDEOCORE_H

#include <exec/libraries.h>
#include <proto/exec.h>

/* Non-zero when videocodec.library opens, so the GUIs offer their VideoCore
 * switch. Whether the VPU takes a stream is mrplay's to find out: it falls
 * back to the software decoder when not. */
static inline int mr_videocore_available(void)
{
    struct Library *lib = OpenLibrary((CONST_STRPTR)"videocodec.library", 0);
    if (lib) CloseLibrary(lib);
    return lib != NULL;
}

#endif /* MR_VIDEOCORE_H */
