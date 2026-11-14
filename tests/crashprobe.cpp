// Q143 crash forensics probe: loads ddraw.dll (its DllMain installs the crash reporter), then lets a thread
// die of an unhandled exception. argv[2]: "raise" = RaiseException(0xE0514444), "av" = write to NULL.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static DWORD WINAPI victim(LPVOID p)
{
    if (strcmp((const char *)p, "av") == 0)
        *(volatile int *)(ULONG_PTR)0x10 = 1;
    else
        RaiseException(0xE0514444, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    SetErrorMode(SEM_NOGPFAULTERRORBOX);   // no Windows error report dialog
    HMODULE dd = LoadLibraryA(argv[1]);
    if (!dd) {
        printf("LoadLibrary failed %lu\n", GetLastError());
        return 3;
    }
    HANDLE t = CreateThread(NULL, 0, victim, argv[2], 0, NULL);
    WaitForSingleObject(t, 30000);
    printf("still alive - the exception did not end the process\n");
    return 1;
}

