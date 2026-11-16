#include <errno.h>
#include <syscall/pxapi.h>

void do_xapi_Broken(char *info)
{
    (void)info;
}

uint64_t do_xapi_SendAppMessage(char *title, char *text)
{
    (void)title;
    (void)text;
    return (uint64_t)-ENOSYS;
}
