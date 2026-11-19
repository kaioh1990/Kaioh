// github.com/sqweek/dialog message boxes (error and retry prompts) on SDL.
#include <stdio.h>
#include "ik/ik_sim.h"

typedef struct {
    GoString msg, title;
} MsgBuilder;

GoOpaque *dialog_Message(GoString format, Slice_GoIface args) {
    MsgBuilder *b = (MsgBuilder *)gc_calloc(1, sizeof *b);
    b->msg = fmt_Sprintf(format, args);
    return (GoOpaque *)b;
}

GoOpaque *dialog_MsgBuilder_Title(GoOpaque *recv, GoString t) {
    ((MsgBuilder *)recv)->title = t;
    return recv;
}

static const char *cz(GoString s) { return gc_strdup(go_strz(s)); }

void dialog_MsgBuilder_Error(GoOpaque *recv) {
    MsgBuilder *b = (MsgBuilder *)recv;
    fprintf(stderr, "%.*s: %.*s\n", b->title.len, b->title.p, b->msg.len, b->msg.p);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, cz(b->title), cz(b->msg), NULL);
}

bool dialog_MsgBuilder_YesNo(GoOpaque *recv) {
    MsgBuilder *b = (MsgBuilder *)recv;
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
    };
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_WARNING, NULL, cz(b->title), cz(b->msg), 2, buttons, NULL};
    int hit = 0;
    if (SDL_ShowMessageBox(&data, &hit) < 0) return false;
    return hit == 1;
}
