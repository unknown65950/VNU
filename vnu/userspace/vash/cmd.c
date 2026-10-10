#include "shell.h"
Word* word_new(int kind)
{
    Word* w = (Word*)malloc(sizeof(Word));
    if (!w)
        return 0;
    memset(w, 0, sizeof(Word));
    w->kind = kind;
    str_init(&w->text);
    return w;
}

Word* word_literal(const char* text)
{
    Word* w = word_new(W_LIT);
    if (w)
        str_adds(&w->text, text);
    return w;
}

void word_free(Word* w)
{
    while (w) {
        Word* n = w->next;
        str_free(&w->text);
        if (w->items) {
            for (int i = 0; i < w->items->n; ++i)
                word_free((Word*)w->items->v[i]);
            vec_free(w->items);
            free(w->items);
        }
        free(w);
        w = n;
    }
}

Cmd* cmd_new(int type)
{
    Cmd* c = (Cmd*)malloc(sizeof(Cmd));
    if (!c)
        return 0;
    memset(c, 0, sizeof(Cmd));
    c->type = type;
    return c;
}

void cmd_free(Cmd* c)
{
    while (c) {
        Cmd* n = c->next;
        if (c->argv) {
            for (int i = 0; i < c->argc; ++i)
                word_free(c->argv[i]);
            free(c->argv);
        }
        Redir* r = c->redirs;
        while (r) {
            Redir* rn = r->next;
            if (r->target)
                word_free(r->target);
            free(r);
            r = rn;
        }
        /* A function definition's body belongs to the function table,
         * not to the command node: the same tree is what a later call
         * runs, and a redefinition frees the old one there. Freeing it
         * here would leave the table with a dangling body. */
        if (c->type != C_FUNC)
            cmd_free(c->left);
        cmd_free(c->right);
        cmd_free(c->init);
        if (c->words) {
            for (int i = 0; i < c->nwords; ++i)
                word_free(c->words[i]);
            free(c->words);
        }
        for (int i = 0; i < c->nlist; ++i)
            cmd_free(c->list[i]);
        free(c->list);
        word_free(c->name);
        str_free(&c->text);
        free(c);
        c = n;
    }
}
