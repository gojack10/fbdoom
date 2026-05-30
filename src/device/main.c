#include <stdio.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

extern void I_PollTouch(void);

int main(int argc, const char** argv)
{
    myargc = argc;
    myargv = argv;

    D_DoomMain();

    return 0;
}

void I_StartTic (void)
{
    I_PollTouch();
}
