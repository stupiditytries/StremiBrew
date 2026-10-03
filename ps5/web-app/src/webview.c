/*
 * Web-view spike: opens a page in the console's built-in web engine through the browser
 * dialog, asking for a full-screen view with no title bar, address bar or footer.
 *
 * The dialog's modules are not loaded into a game process by default and the system-module
 * loader rejected the PS4-era module number, so they are loaded by file name and their
 * functions looked up by name. The structures follow the dialog's PS4-era interface;
 * whether the PS5 module accepts them unchanged is one of the things this spike finds out,
 * so every call's result is logged and nothing is assumed to succeed.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int sceUserServiceInitialize(void *parameters);
int sceUserServiceGetInitialUser(int32_t *user);
int sceKernelLoadStartModule(const char *path, size_t argument_size, const void *argument,
                             uint32_t flags, void *options, int *result);
int sceKernelDlsym(int module, const char *symbol, void **address);
const char *sceKernelGetFsSandboxRandomWord(void);

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
static int (*dialog_update_status)(void);
static int last_status = -100;

static void report(const char *step, long result)
{
    char line[112];
    snprintf(line, sizeof line, "WEB %s = %ld (0x%08lx)", step, result,
             (unsigned long)(uint32_t)result);
    log_line(line);
}

/* Loads a system module by file name, trying the folders it can live in. Returns its
 * handle, or a negative error. */
static int load_module(const char *name)
{
    char path[160];
    char step[160];
    int handle = -1;
    const char *word = sceKernelGetFsSandboxRandomWord();
    const char *folders[] = {"/system/common/lib", "/%s/common/lib", "/system_ex/common_ex/lib"};
    for (unsigned index = 0; index < sizeof folders / sizeof folders[0]; ++index)
    {
        char folder[96];
        snprintf(folder, sizeof folder, folders[index], word != NULL ? word : "system");
        snprintf(path, sizeof path, "%s/%s", folder, name);
        handle = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
        snprintf(step, sizeof step, "load %s", path);
        report(step, handle);
        if (handle >= 0)
            return handle;
    }
    return handle;
}

static void *lookup(int module, const char *symbol)
{
    void *address = NULL;
    int result = sceKernelDlsym(module, symbol, &address);
    if (result != 0 || address == NULL)
    {
        char step[96];
        snprintf(step, sizeof step, "lookup %s", symbol);
        report(step, result);
        return NULL;
    }
    return address;
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

    int32_t user = 0;
    report("sceUserServiceInitialize", sceUserServiceInitialize(NULL));
    report("sceUserServiceGetInitialUser", sceUserServiceGetInitialUser(&user));

    int common = load_module("libSceCommonDialog.sprx");
    int browser = load_module("libSceWebBrowserDialog.sprx");
    if (common < 0 || browser < 0)
    {
        log("WEB the dialog modules could not be loaded");
        return -1;
    }
    int (*common_initialize)(void) = lookup(common, "sceCommonDialogInitialize");
    int (*initialize)(void) = lookup(browser, "sceWebBrowserDialogInitialize");
    int (*open)(const void *) = lookup(browser, "sceWebBrowserDialogOpen");
    dialog_update_status = lookup(browser, "sceWebBrowserDialogUpdateStatus");
    if (common_initialize == NULL || initialize == NULL || open == NULL ||
        dialog_update_status == NULL)
    {
        log("WEB the dialog functions could not be found");
        return -1;
    }

    report("sceCommonDialogInitialize", common_initialize());
    report("sceWebBrowserDialogInitialize", initialize());

    fill(&param, user, url, MODE_CUSTOM);
    int opened = open(&param);
    report("open full screen no bars", opened);
    if (opened != 0)
    {
        /* Fall back to the standard dialog, to learn whether the dialog works at all. */
        fill(&param, user, url, MODE_DEFAULT);
        opened = open(&param);
        report("open standard", opened);
    }
    return opened;
}

/* Call once per frame while the dialog is open; logs each change of its status. */
void webview_update(void)
{
    if (log_line == NULL || dialog_update_status == NULL)
        return;
    int status = dialog_update_status();
    if (status != last_status)
    {
        last_status = status;
        report("status", status);
    }
}
