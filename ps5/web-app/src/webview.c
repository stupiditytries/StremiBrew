/*
 * Web-view spike: opens a page in the console's built-in web engine through the browser
 * dialog, asking for a full-screen view with no title bar, address bar or footer.
 *
 * The structures follow the dialog's PS4-era interface; whether the PS5 module accepts
 * them unchanged is one of the things this spike finds out, so every call's result is
 * logged and nothing is assumed to succeed.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMPORT

/* Whether the loader resolved an imported function. An import from a module that is not
 * loaded is a null pointer; the address is read through a volatile so the compiler cannot
 * assume it is non-null. */
static int resolved(void *volatile function)
{
    return function != NULL;
}

IMPORT int sceUserServiceInitialize(void *parameters);
IMPORT int sceUserServiceGetInitialUser(int32_t *user);
IMPORT int sceSysmoduleLoadModule(uint32_t module);
IMPORT int sceSysmoduleLoadModuleInternal(uint32_t module);
IMPORT int sceCommonDialogInitialize(void);
IMPORT int sceWebBrowserDialogInitialize(void);
IMPORT int sceWebBrowserDialogOpen(const void *parameters);
IMPORT int sceWebBrowserDialogUpdateStatus(void);
IMPORT int sceWebBrowserDialogGetStatus(void);
IMPORT int sceWebBrowserDialogClose(void);
IMPORT int sceWebBrowserDialogTerminate(void);

#define MODULE_WEB_BROWSER_DIALOG 0x0080u

#define COMMON_DIALOG_MAGIC 0xC0D1A109u

struct common_dialog_base
{
    size_t size;
    uint8_t reserved[36];
    uint32_t magic;
} __attribute__((aligned(8)));

#define MODE_DEFAULT 1
#define MODE_CUSTOM 2
#define PARTS_NONE 0u
#define CONTROL_EXIT 1u
#define ANIMATION_DISABLE 1u

struct web_browser_dialog_param
{
    struct common_dialog_base base;
    size_t size;
    int32_t mode;
    int32_t user;
    const char *url;
    void *callback_parameters;
    uint16_t width;
    uint16_t height;
    uint16_t position_x;
    uint16_t position_y;
    uint32_t parts;
    uint16_t header_width;
    uint16_t header_position_x;
    uint16_t header_position_y;
    uint8_t padding[2];
    uint32_t control;
    void *ime_parameters;
    void *web_view_parameters;
    uint32_t animation;
    uint8_t reserved[202];
};

static void (*log_line)(const char *);
static int last_status = -100;

static void report(const char *step, long result)
{
    char line[96];
    snprintf(line, sizeof line, "WEB %s = %ld (0x%08lx)", step, result,
             (unsigned long)(uint32_t)result);
    log_line(line);
}

static void fill(struct web_browser_dialog_param *param, int32_t user, const char *url,
                 int mode)
{
    memset(param, 0, sizeof *param);
    param->base.size = sizeof param->base;
    param->base.magic = (uint32_t)(COMMON_DIALOG_MAGIC + (uintptr_t)param);
    param->size = sizeof *param;
    param->mode = mode;
    param->user = user;
    param->url = url;
    if (mode == MODE_CUSTOM)
    {
        param->width = 1920;
        param->height = 1080;
        param->position_x = 0;
        param->position_y = 0;
        param->parts = PARTS_NONE;
        param->control = CONTROL_EXIT;
        param->animation = ANIMATION_DISABLE;
    }
}

/* Loads and initialises the dialog and opens `url`. Returns 0 when a dialog was opened. */
int webview_open(const char *url, void (*log)(const char *))
{
    static struct web_browser_dialog_param param;
    log_line = log;

    char line[96];
    snprintf(line, sizeof line, "WEB param sizes: base %zu, dialog %zu", sizeof param.base,
             sizeof param);
    log(line);

    if (!resolved((void *)sceSysmoduleLoadModule) || !resolved((void *)sceWebBrowserDialogOpen))
        log("WEB note: some imports are unresolved before the module is loaded");

    int32_t user = 0;
    if (resolved((void *)sceUserServiceInitialize))
        report("sceUserServiceInitialize", sceUserServiceInitialize(NULL));
    if (resolved((void *)sceUserServiceGetInitialUser))
        report("sceUserServiceGetInitialUser", sceUserServiceGetInitialUser(&user));
    report("user", user);

    if (resolved((void *)sceSysmoduleLoadModule))
        report("sceSysmoduleLoadModule", sceSysmoduleLoadModule(MODULE_WEB_BROWSER_DIALOG));
    if (!resolved((void *)sceCommonDialogInitialize) || !resolved((void *)sceWebBrowserDialogInitialize) ||
        !resolved((void *)sceWebBrowserDialogOpen) || !resolved((void *)sceWebBrowserDialogUpdateStatus))
    {
        log("WEB dialog functions are still unresolved after loading the module");
        return -1;
    }
    report("sceCommonDialogInitialize", sceCommonDialogInitialize());
    report("sceWebBrowserDialogInitialize", sceWebBrowserDialogInitialize());

    fill(&param, user, url, MODE_CUSTOM);
    int opened = sceWebBrowserDialogOpen(&param);
    report("sceWebBrowserDialogOpen full screen, no bars", opened);
    if (opened != 0)
    {
        /* Fall back to the standard dialog, to learn whether the dialog works at all. */
        fill(&param, user, url, MODE_DEFAULT);
        opened = sceWebBrowserDialogOpen(&param);
        report("sceWebBrowserDialogOpen standard", opened);
    }
    return opened;
}

/* Call once per frame while the dialog is open; logs each change of its status. */
void webview_update(void)
{
    if (log_line == NULL || !resolved((void *)sceWebBrowserDialogUpdateStatus))
        return;
    int status = sceWebBrowserDialogUpdateStatus();
    if (status != last_status)
    {
        last_status = status;
        report("status", status);
    }
}
