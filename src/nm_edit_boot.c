#include <nm_core.h>
#include <nm_form.h>
#include <nm_utils.h>
#include <nm_string.h>
#include <nm_window.h>
#include <nm_database.h>
#include <nm_vm_control.h>

#define NM_BOOT_FIELDS_NUM 7

static nm_field_t *fields[NM_BOOT_FIELDS_NUM + 1];

enum {
    NM_FLD_INST = 0,
    NM_FLD_SRCP,
    NM_FLD_BIOS,
    NM_FLD_KERN,
    NM_FLD_CMDL,
    NM_FLD_TTYP,
    NM_FLD_SOCK
};

static void nm_edit_boot_field_setup(const nm_vmctl_data_t *cur);
static void nm_edit_boot_field_names(const nm_str_t *name, nm_window_t *w);
static int nm_edit_boot_get_data(nm_vm_boot_t *vm);
static void nm_edit_boot_update_db(const nm_str_t *name, nm_vm_boot_t *vm);

void nm_edit_boot(const nm_str_t *name)
{
    nm_form_t *form = NULL;
    nm_window_t *window = NULL;
    nm_vm_boot_t vm = NM_INIT_VM_BOOT;
    nm_vmctl_data_t cur_settings = NM_VMCTL_INIT_DATA;
    nm_spinner_data_t sp_data = NM_INIT_SPINNER;
    size_t msg_len;
    pthread_t spin_th;
    int done = 0;

    nm_vmctl_get_data(name, &cur_settings);

    nm_print_title(_(NM_EDIT_TITLE));
    window = nm_init_window(19, 67, 3);

    init_pair(1, COLOR_BLACK, COLOR_WHITE);
    wbkgd(window, COLOR_PAIR(1));

    for (size_t n = 0; n < NM_BOOT_FIELDS_NUM; ++n)
    {
        fields[n] = new_field(1, 41, (n + 1) * 2, 5, 0, 0);
        set_field_back(fields[n], A_UNDERLINE);
    }

    fields[NM_BOOT_FIELDS_NUM] = NULL;

    nm_edit_boot_field_setup(&cur_settings);
    nm_edit_boot_field_names(name, window);

    form = nm_post_form(window, fields, 18);
    if (nm_draw_form(window, form) != NM_OK)
        goto out;

    if (nm_edit_boot_get_data(&vm) != NM_OK)
        goto out;

    msg_len = mbstowcs(NULL, _(NM_EDIT_TITLE), strlen(_(NM_EDIT_TITLE)));
    sp_data.stop = &done;
    sp_data.x = (getmaxx(stdscr) + msg_len + 2) / 2;

    if (pthread_create(&spin_th, NULL, nm_spinner, (void *) &sp_data) != 0)
        nm_bug(_("%s: cannot create thread"), __func__);

    nm_edit_boot_update_db(name, &vm);

    done = 1;
    if (pthread_join(spin_th, NULL) != 0)
        nm_bug(_("%s: cannot join thread"), __func__);

out:
    nm_vmctl_free_data(&cur_settings);
    nm_vm_free_boot(&vm);
    nm_form_free(form, fields);
}

static void nm_edit_boot_field_setup(const nm_vmctl_data_t *cur)
{
    for (size_t n = 1; n < NM_BOOT_FIELDS_NUM; n++)
        field_opts_off(fields[n], O_STATIC);

    set_field_type(fields[NM_FLD_INST], TYPE_ENUM, nm_form_yes_no, false, false);
    set_field_type(fields[NM_FLD_SRCP], TYPE_REGEXP, "^/.*");
    set_field_type(fields[NM_FLD_BIOS], TYPE_REGEXP, "^/.*");
    set_field_type(fields[NM_FLD_KERN], TYPE_REGEXP, "^/.*");
    set_field_type(fields[NM_FLD_CMDL], TYPE_REGEXP, ".*");
    set_field_type(fields[NM_FLD_TTYP], TYPE_REGEXP, "^/.*");
    set_field_type(fields[NM_FLD_SOCK], TYPE_REGEXP, "^/.*");

    if (nm_str_cmp_st(nm_vect_str(&cur->main, NM_SQL_INST), NM_ENABLE) == NM_OK)
        set_field_buffer(fields[NM_FLD_INST], 0, nm_form_yes_no[1]);
    else
        set_field_buffer(fields[NM_FLD_INST], 0, nm_form_yes_no[0]);

    set_field_buffer(fields[NM_FLD_SRCP], 0, nm_vect_str_ctx(&cur->main, NM_SQL_ISO));
    set_field_buffer(fields[NM_FLD_BIOS], 0, nm_vect_str_ctx(&cur->main, NM_SQL_BIOS));
    set_field_buffer(fields[NM_FLD_KERN], 0, nm_vect_str_ctx(&cur->main, NM_SQL_KERN));
    set_field_buffer(fields[NM_FLD_CMDL], 0, nm_vect_str_ctx(&cur->main, NM_SQL_KAPP));
    set_field_buffer(fields[NM_FLD_TTYP], 0, nm_vect_str_ctx(&cur->main, NM_SQL_TTY));
    set_field_buffer(fields[NM_FLD_SOCK], 0, nm_vect_str_ctx(&cur->main, NM_SQL_SOCK));

    for (size_t n = 0; n < NM_BOOT_FIELDS_NUM; n++)
        set_field_status(fields[n], 0);
}

static void nm_edit_boot_field_names(const nm_str_t *name, nm_window_t *w)
{
    nm_str_t buf = NM_INIT_STR;

    nm_str_alloc_str(&buf, name);
    nm_str_add_text(&buf, _(" boot settings"));

    mvwaddstr(w, 1, 2,  buf.data);
    mvwaddstr(w, 4, 2,  _("OS Installed"));
    mvwaddstr(w, 6, 2,  _("Path to ISO/IMG"));
    mvwaddstr(w, 8, 2,  _("Path to BIOS"));
    mvwaddstr(w, 10, 2, _("Path to kernel"));
    mvwaddstr(w, 12, 2, _("Kernel cmdline"));
    mvwaddstr(w, 14, 2, _("Serial TTY"));
    mvwaddstr(w, 16, 2, _("Serial socket"));

    nm_str_free(&buf);
}

static int nm_edit_boot_get_data(nm_vm_boot_t *vm)
{
    int rc = NM_OK;
    nm_vect_t err = NM_INIT_VECT;
    nm_str_t inst = NM_INIT_STR;

    nm_get_field_buf(fields[NM_FLD_INST], &inst);
    nm_get_field_buf(fields[NM_FLD_SRCP], &vm->inst_path);
    nm_get_field_buf(fields[NM_FLD_BIOS], &vm->bios);
    nm_get_field_buf(fields[NM_FLD_KERN], &vm->kernel);
    nm_get_field_buf(fields[NM_FLD_CMDL], &vm->cmdline);
    nm_get_field_buf(fields[NM_FLD_TTYP], &vm->tty);
    nm_get_field_buf(fields[NM_FLD_CMDL], &vm->cmdline);

    if (field_status(fields[NM_FLD_INST]))
        nm_form_check_data(_("OS Installed"), inst, err);

    if ((rc = nm_print_empty_fields(&err)) == NM_ERR)
        goto out;

    if (nm_str_cmp_st(&inst, "yes") == NM_OK)
    {
        vm->installed = 1;

        if (vm->inst_path.len == 0)
        {
            nm_print_warn(3, 6, _("ISO/IMG path not set"));
            rc = NM_ERR;
            goto out;
        }
    }

out:
    nm_str_free(&inst);
    nm_vect_free(&err, NULL);

    return rc;
}

static void nm_edit_boot_update_db(const nm_str_t *name, nm_vm_boot_t *vm)
{
    nm_str_t query = NM_INIT_STR;

    if (field_status(fields[NM_FLD_INST]))
    {
        nm_str_alloc_text(&query, "UPDATE vms SET install='");
        nm_str_add_text(&query, vm->installed ? NM_DISABLE : NM_ENABLE);
        nm_str_add_text(&query, "' WHERE name='");
        nm_str_add_str(&query, name);
        nm_str_add_char(&query, '\'');
        nm_db_edit(query.data);
        nm_str_trunc(&query, 0);
    }

    nm_str_free(&query);
}

/* vim:set ts=4 sw=4 fdm=marker: */