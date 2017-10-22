#include <nm_core.h>

#if defined (NM_WITH_OVF_SUPPORT)
#include <nm_form.h>
#include <nm_utils.h>
#include <nm_string.h>
#include <nm_vector.h>
#include <nm_window.h>
#include <nm_add_vm.h>
#include <nm_cfg_file.h>
#include <nm_ovf_import.h>

#include <limits.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

/* libxml2 */
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#define NM_OVA_DIR_TEMPL "/tmp/ova_extract_XXXXXX"
#define NM_BLOCK_SIZE 10240
#define NM_OVF_FIELDS_NUM 3

#define NM_OVF_FORM_PATH "Path to OVA"
#define NM_OVF_FORM_ARCH "Architecture"
#define NM_OVF_FORM_NAME "Name (optional)"

#define NM_XML_OVF_NS "ovf"
#define NM_XML_RASD_NS "rasd"
#define NM_XML_OVF_HREF "http://schemas.dmtf.org/ovf/envelope/1"
#define NM_XML_RASD_HREF "http://schemas.dmtf.org/wbem/wscim/1" \
    "/cim-schema/2/CIM_ResourceAllocationSettingData"
#define NM_XPATH_NAME "/ovf:Envelope/ovf:VirtualSystem/ovf:Name/text()"
#define NM_XPATH_MEM  "/ovf:Envelope/ovf:VirtualSystem/ovf:VirtualHardwareSection/" \
    "ovf:Item[rasd:ResourceType/text()=4]/rasd:VirtualQuantity/text()"
#define NM_XPATH_NCPU "/ovf:Envelope/ovf:VirtualSystem/ovf:VirtualHardwareSection/" \
    "ovf:Item[rasd:ResourceType/text()=3]/rasd:VirtualQuantity/text()"
#define NM_XPATH_DRIVE_ID "/ovf:Envelope/ovf:VirtualSystem/ovf:VirtualHardwareSection/" \
    "ovf:Item[rasd:ResourceType/text()=17]/rasd:HostResource/text()"
#define NM_XPATH_DRIVE_REF "/ovf:Envelope/ovf:DiskSection/" \
    "ovf:Disk[@ovf:diskId=\"%s\"]/@ovf:fileRef"
#define NM_XPATH_DRIVE_CAP "/ovf:Envelope/ovf:DiskSection/" \
    "ovf:Disk[@ovf:diskId=\"%s\"]/@ovf:capacity"
#define NM_XPATH_DRIVE_HREF "/ovf:Envelope/ovf:References/" \
    "ovf:File[@ovf:id=\"%s\"]/@ovf:href"
#define NM_XPATH_NETH "/ovf:Envelope/ovf:VirtualSystem/ovf:VirtualHardwareSection/" \
    "ovf:Item[rasd:ResourceType/text()=10]"

#define NM_QEMU_CONVERT "/bin/qemu-img convert -O qcow2"

enum {
    NM_OVA_FLD_SRC = 0,
    NM_OVA_FLD_ARCH,
    NM_OVA_FLD_NAME
};

typedef struct archive nm_archive_t;
typedef struct archive_entry nm_archive_entry_t;

typedef xmlChar nm_xml_char_t;
typedef xmlDocPtr nm_xml_doc_pt;
typedef xmlNodePtr nm_xml_node_pt;
typedef xmlNodeSetPtr nm_xml_node_set_pt;
typedef xmlXPathObjectPtr nm_xml_xpath_obj_pt;
typedef xmlXPathContextPtr nm_xml_xpath_ctx_pt;

void nm_drive_vect_ins_cb(const void *unit_p, const void *ctx);
void nm_drive_vect_free_cb(const void *unit_p);

static int nm_clean_temp_dir(const char *tmp_dir, const nm_vect_t *files);
static void nm_archive_copy_data(nm_archive_t *in, nm_archive_t *out);
static void nm_ovf_extract(const nm_str_t *ova_path, const char *tmp_dir,
                           nm_vect_t *files);
static const char *nm_find_ovf(const nm_vect_t *files);
static nm_xml_doc_pt nm_ovf_open(const char *tmp_dir, const char *ovf_file);
static int nm_register_xml_ns(nm_xml_xpath_ctx_pt ctx);
static int __nm_register_xml_ns(nm_xml_xpath_ctx_pt ctx, const char *ns,
                                const char *href);
static nm_xml_xpath_obj_pt nm_exec_xpath(const char *xpath,
                                         nm_xml_xpath_ctx_pt ctx);
static void nm_ovf_get_name(nm_str_t *name, nm_xml_xpath_ctx_pt ctx);
static void nm_ovf_get_ncpu(nm_str_t *ncpu, nm_xml_xpath_ctx_pt ctx);
static void nm_ovf_get_mem(nm_str_t *mem, nm_xml_xpath_ctx_pt ctx);
static void nm_ovf_get_drives(nm_vect_t *drives, nm_xml_xpath_ctx_pt ctx);
static uint32_t nm_ovf_get_neth(nm_xml_xpath_ctx_pt ctx);
static void nm_ovf_get_text(nm_str_t *res, nm_xml_xpath_ctx_pt ctx,
                            const char *xpath, const char *param);
static inline void nm_drive_free(nm_drive_t *d);
static void nm_ovf_convert_drives(const nm_vect_t *drives, const nm_str_t *name,
                                  const char *templ_path);
static void nm_ovf_to_db(nm_vm_t *vm, const nm_vect_t *drives);
static int nm_ova_get_data(nm_vm_t *vm);

static nm_field_t *fields[NM_OVF_FIELDS_NUM + 1];

void nm_ovf_import(void)
{
    char templ_path[] = NM_OVA_DIR_TEMPL;
    const char *ovf_file;
    nm_vect_t files = NM_INIT_VECT;
    nm_vect_t drives = NM_INIT_VECT;
    nm_vm_t vm = NM_INIT_VM;
    nm_xml_doc_pt doc = NULL;
    nm_xml_xpath_ctx_pt xpath_ctx = NULL;
    nm_window_t *window = NULL;
    nm_form_t *form = NULL;
    nm_spinner_data_t sp_data = NM_INIT_SPINNER;
    size_t msg_len;
    pthread_t spin_th;
    int done = 0;

    nm_print_title(_(NM_EDIT_TITLE));
    window = nm_init_window(9, 67, 3);

    init_pair(1, COLOR_BLACK, COLOR_WHITE);
    wbkgd(window, COLOR_PAIR(1));

    for (size_t n = 0; n < NM_OVF_FIELDS_NUM; ++n)
    {
        fields[n] = new_field(1, 38, n * 2, 5, 0, 0);
        set_field_back(fields[n], A_UNDERLINE);
    }
    fields[NM_OVF_FIELDS_NUM] = NULL;

    set_field_type(fields[NM_OVA_FLD_SRC], TYPE_REGEXP, "^/.*");
    set_field_type(fields[NM_OVA_FLD_ARCH], TYPE_ENUM,
                   nm_cfg_get_arch(), false, false);
    set_field_type(fields[NM_OVA_FLD_NAME], TYPE_REGEXP,
                   "^[a-zA-Z0-9_-]{1,30} *$");
    set_field_buffer(fields[NM_OVA_FLD_ARCH], 0, *nm_cfg_get()->qemu_targets.data);

    mvwaddstr(window, 2, 2, _(NM_OVF_FORM_PATH));
    mvwaddstr(window, 4, 2, _(NM_OVF_FORM_ARCH));
    mvwaddstr(window, 6, 2, _(NM_OVF_FORM_NAME));

    form = nm_post_form(window, fields, 21);
    if (nm_draw_form(window, form) != NM_OK)
        goto cancel;

    if (nm_ova_get_data(&vm) != NM_OK)
        goto out;

    msg_len = mbstowcs(NULL, _(NM_EDIT_TITLE), strlen(_(NM_EDIT_TITLE)));
    sp_data.stop = &done;
    sp_data.x = (getmaxx(stdscr) + msg_len + 2) / 2;

    if (pthread_create(&spin_th, NULL, nm_spinner, (void *) &sp_data) != 0)
        nm_bug(_("%s: cannot create thread"), __func__);

    if (mkdtemp(templ_path) == NULL)
        nm_bug("%s: mkdtemp error: %s", __func__, strerror(errno));

    nm_ovf_extract(&vm.srcp, templ_path, &files);

    if ((ovf_file = nm_find_ovf(&files)) == NULL)
    {
        nm_print_warn(3, 6, _("OVF file is not found!"));
        goto out;
    }

#if NM_DEBUG
    nm_debug("ova: ovf file found: %s\n", ovf_file);
#endif

    if ((doc = nm_ovf_open(templ_path, ovf_file)) == NULL)
    {
        nm_print_warn(3, 6, _("Cannot parse OVF file"));
        goto out;
    }

    if ((xpath_ctx = xmlXPathNewContext(doc)) == NULL)
    {
        nm_print_warn(3, 6, _("Cannot create new XPath context"));
        goto out;
    }

    if (nm_register_xml_ns(xpath_ctx) != NM_OK)
    {
        nm_print_warn(3, 6, _("Cannot register xml namespaces"));
        goto out;
    }

    if (vm.name.len == 0)
        nm_ovf_get_name(&vm.name, xpath_ctx);
    nm_ovf_get_ncpu(&vm.cpus, xpath_ctx);
    nm_ovf_get_mem(&vm.memo, xpath_ctx);
    nm_ovf_get_drives(&drives, xpath_ctx);
    vm.ifs.count = nm_ovf_get_neth(xpath_ctx);

    if (nm_form_name_used(&vm.name) != NM_OK)
        goto out;

    nm_ovf_convert_drives(&drives, &vm.name, templ_path);
    nm_ovf_to_db(&vm, &drives);

    done = 1;
    if (pthread_join(spin_th, NULL) != 0)
        nm_bug(_("%s: cannot join thread"), __func__);

out:
    if (nm_clean_temp_dir(templ_path, &files) != NM_OK)
        nm_print_warn(3, 6, _("Some files was not deleted"));

    xmlXPathFreeContext(xpath_ctx);
    xmlFreeDoc(doc);
    xmlCleanupParser();

    nm_vect_free(&files, NULL);
    nm_vect_free(&drives, nm_drive_vect_free_cb);

cancel:
    nm_form_free(form, fields);
    nm_vm_free(&vm);
}

static void nm_ovf_extract(const nm_str_t *ova_path, const char *tmp_dir,
                           nm_vect_t *files)
{
    nm_archive_t *in, *out;
    nm_archive_entry_t *ar_entry;
    char cwd[PATH_MAX] = {0};
    int rc;

    in = archive_read_new();
    out = archive_write_disk_new();

    archive_write_disk_set_options(out, 0);
    archive_read_support_format_tar(in);

    if (archive_read_open_filename(in, ova_path->data, NM_BLOCK_SIZE) != 0)
        nm_bug("%s: ovf read error: %s", __func__, archive_error_string(in));

    /* save current working directory */
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        nm_bug("%s: getcwd error: %s", __func__, strerror(errno));

    /* change current working directory to temporary */
    if (chdir(tmp_dir) == -1)
        nm_bug("%s: chdir error: %s", __func__, strerror(errno));

    for (;;)
    {
        const char *file;
        rc = archive_read_next_header(in, &ar_entry);

        if (rc == ARCHIVE_EOF)
            break;

        if (rc != ARCHIVE_OK)
            nm_bug("%s: bad archive: %s", __func__, archive_error_string(in));

        file = archive_entry_pathname(ar_entry);

        nm_vect_insert(files, file, strlen(file) + 1, NULL);
#ifdef NM_DEBUG
        nm_debug("ova: extract file: %s\n", file);
#endif
        rc = archive_write_header(out, ar_entry);
        if (rc != ARCHIVE_OK)
            nm_bug("%s: bad archive: %s", __func__, archive_error_string(in));
        else
        {
            nm_archive_copy_data(in, out);
            if (archive_write_finish_entry(out) != ARCHIVE_OK)
            {
                nm_bug("%s: archive_write_finish_entry: %s",
                       __func__, archive_error_string(out));
            }
        }
    }

    archive_read_free(in);
    archive_write_free(out);

    /* restore current working directory */
    if (chdir(cwd) == -1)
        nm_bug("%s: chdir error: %s", __func__, strerror(errno));
}

static void nm_archive_copy_data(nm_archive_t *in, nm_archive_t *out)
{
    int rc;
    const void *buf;
    size_t size;
#if ARCHIVE_VERSION_NUMBER >= 3000000
    int64_t offset;
#else
    off_t offset;
#endif

    for (;;)
    {
        rc = archive_read_data_block(in, &buf, &size, &offset);
        if (rc == ARCHIVE_EOF)
            break;
        if (rc != ARCHIVE_OK)
            nm_bug("%s: error read archive: %s", __func__, archive_error_string(in));

        if (archive_write_data_block(out, buf, size, offset) != ARCHIVE_OK)
            nm_bug("%s: error write archive: %s", __func__, archive_error_string(out));
    }
}

static int nm_clean_temp_dir(const char *tmp_dir, const nm_vect_t *files)
{
    nm_str_t path = NM_INIT_STR;
    int rc = NM_OK;

    for (size_t n = 0; n < files->n_memb; n++)
    {
        nm_str_format(&path, "%s/%s", tmp_dir, (char *) nm_vect_at(files, n));

        if (unlink(path.data) == -1)
            rc = NM_ERR;
#ifdef NM_DEBUG
        nm_debug("ova: clean file: %s\n", path.data);
#endif
        nm_str_trunc(&path, 0);
    }

    if (rc == NM_OK)
    {
        if (rmdir(tmp_dir) == -1)
            rc = NM_ERR;
    }

    nm_str_free(&path);

    return rc;
}

static const char *nm_find_ovf(const nm_vect_t *files)
{
    for (size_t n = 0; n < files->n_memb; n++)
    {
        const char *file = nm_vect_at(files, n);
        size_t len = strlen(file);

        if (len < 4)
            continue;

        if (nm_str_cmp_tt(file + (len - 4), ".ovf") == NM_OK)
            return file;
    }

    return NULL;
}

static nm_xml_doc_pt nm_ovf_open(const char *tmp_dir, const char *ovf_file)
{
    nm_xml_doc_pt doc = NULL;
    nm_str_t ovf_path = NM_INIT_STR;

    nm_str_format(&ovf_path, "%s/%s", tmp_dir, ovf_file);

    doc = xmlParseFile(ovf_path.data);

    nm_str_free(&ovf_path);

    return doc;
}

static int nm_register_xml_ns(nm_xml_xpath_ctx_pt ctx)
{
    if ((__nm_register_xml_ns(ctx, NM_XML_OVF_NS, NM_XML_OVF_HREF) == NM_ERR) ||
        (__nm_register_xml_ns(ctx, NM_XML_RASD_NS, NM_XML_RASD_HREF) == NM_ERR))
    {
        return NM_ERR;
    }

    return NM_OK;
}

static int __nm_register_xml_ns(nm_xml_xpath_ctx_pt ctx, const char *ns,
                                const char *href)
{
    int rc = NM_OK;

    nm_xml_char_t *xml_ns;
    nm_xml_char_t *xml_href;

    if ((xml_ns = xmlCharStrdup(ns)) == NULL)
        return NM_ERR;

    if ((xml_href = xmlCharStrdup(href)) == NULL)
    {
        xmlFree(xml_ns);
        return NM_ERR;
    }

    if (xmlXPathRegisterNs(ctx, xml_ns, xml_href) != 0)
        rc = NM_ERR;

    xmlFree(xml_ns);
    xmlFree(xml_href);

    return rc;
}

static nm_xml_xpath_obj_pt nm_exec_xpath(const char *xpath,
                                         nm_xml_xpath_ctx_pt ctx)
{
    nm_xml_char_t *xml_xpath;
    nm_xml_xpath_obj_pt obj = NULL;

    if ((xml_xpath = xmlCharStrdup(xpath)) == NULL)
        return NULL;

    obj = xmlXPathEvalExpression(xml_xpath, ctx);

    xmlFree(xml_xpath);

    return obj;
}

static void nm_ovf_get_mem(nm_str_t *mem, nm_xml_xpath_ctx_pt ctx)
{
    nm_ovf_get_text(mem, ctx, NM_XPATH_MEM, "memory");
}

static void nm_ovf_get_name(nm_str_t *name, nm_xml_xpath_ctx_pt ctx)
{
    nm_ovf_get_text(name, ctx, NM_XPATH_NAME, "name");
}

static void nm_ovf_get_ncpu(nm_str_t *ncpu, nm_xml_xpath_ctx_pt ctx)
{
    nm_ovf_get_text(ncpu, ctx, NM_XPATH_NCPU, "ncpu");
}

static void nm_ovf_get_drives(nm_vect_t *drives, nm_xml_xpath_ctx_pt ctx)
{
    nm_xml_xpath_obj_pt obj_id;
    size_t ndrives;

    if ((obj_id = nm_exec_xpath(NM_XPATH_DRIVE_ID, ctx)) == NULL)
        nm_bug("%s: cannot get drive_id from ovf file", __func__);

    if ((ndrives = obj_id->nodesetval->nodeNr) == 0)
        nm_bug("%s: no drives was found", __func__);

    for (size_t n = 0; n < ndrives; n++)
    {
        nm_xml_node_pt node_id = obj_id->nodesetval->nodeTab[n];

        if ((node_id->type == XML_TEXT_NODE) || (node_id->type == XML_ATTRIBUTE_NODE))
        {
            nm_xml_char_t *xml_id = xmlNodeGetContent(node_id);
            nm_str_t xpath = NM_INIT_STR;
            nm_str_t buf = NM_INIT_STR;
            nm_drive_t drive = NM_INIT_DRIVE;

            char *id = strrchr((char *) xml_id, '/');

            if (id == NULL)
                nm_bug("%s: NULL disk id", __func__);

            id++;
#ifdef NM_DEBUG
            nm_debug("ova: drive_id: %s\n", id);
#endif

            nm_str_format(&xpath, NM_XPATH_DRIVE_CAP, id);
            nm_ovf_get_text(&buf, ctx, xpath.data, "capacity");
            nm_str_copy(&drive.capacity, &buf);

            nm_str_trunc(&xpath, 0);
            nm_str_trunc(&buf, 0);

            nm_str_format(&xpath, NM_XPATH_DRIVE_REF, id);
            nm_ovf_get_text(&buf, ctx, xpath.data, "file ref");

            nm_str_trunc(&xpath, 0);

            nm_str_format(&xpath, NM_XPATH_DRIVE_HREF, buf.data);
            nm_str_trunc(&buf, 0);
            nm_ovf_get_text(&buf, ctx, xpath.data, "file href");
            nm_str_copy(&drive.file_name, &buf);

            nm_vect_insert(drives, &drive, sizeof(nm_drive_t),
                           nm_drive_vect_ins_cb);

            xmlFree(xml_id);
            nm_str_free(&xpath);
            nm_str_free(&buf);
            nm_drive_free(&drive);
        }
    }

    xmlXPathFreeObject(obj_id);
}

static uint32_t nm_ovf_get_neth(nm_xml_xpath_ctx_pt ctx)
{
    uint32_t neth = 0;
    nm_xml_xpath_obj_pt obj;

    if ((obj = nm_exec_xpath(NM_XPATH_NETH, ctx)) == NULL)
        nm_bug("%s: cannot get interfaces from ovf file", __func__);

    neth = obj->nodesetval->nodeNr;
#ifdef NM_DEBUG
    nm_debug("ova: eth num: %u\n", neth);
#endif

    xmlXPathFreeObject(obj);

    return neth;
}

static void nm_ovf_get_text(nm_str_t *res, nm_xml_xpath_ctx_pt ctx,
                            const char *xpath, const char *param)
{
    nm_xml_xpath_obj_pt obj;
    nm_xml_node_pt node;
    nm_xml_char_t *xml_text;

    if ((obj = nm_exec_xpath(xpath, ctx)) == NULL)
        nm_bug("%s: cannot get %s from ovf file", __func__, param);

    if (obj->nodesetval->nodeNr == 0)
        nm_bug("%s: xpath return zero result", __func__);

    node = obj->nodesetval->nodeTab[0];

    if ((node->type == XML_TEXT_NODE) || (node->type == XML_ATTRIBUTE_NODE))
    {
        xml_text = xmlNodeGetContent(node);
        nm_str_alloc_text(res, (char *) xml_text);
        xmlFree(xml_text);
    }

#ifdef NM_DEBUG
    nm_debug("ova: %s: %s\n", param, res->data);
#endif

    xmlXPathFreeObject(obj);
}

void nm_drive_vect_ins_cb(const void *unit_p, const void *ctx)
{
    nm_str_copy(&nm_drive_file(unit_p), &nm_drive_file(ctx));
    nm_str_copy(&nm_drive_size(unit_p), &nm_drive_size(ctx));
}

void nm_drive_vect_free_cb(const void *unit_p)
{
    nm_str_free(&nm_drive_file(unit_p));
    nm_str_free(&nm_drive_size(unit_p));
}

static inline void nm_drive_free(nm_drive_t *d)
{
    nm_str_free(&d->file_name);
    nm_str_free(&d->capacity);
}

static void nm_ovf_convert_drives(const nm_vect_t *drives, const nm_str_t *name,
                                  const char *templ_path)
{
    nm_str_t vm_dir = NM_INIT_STR;
    nm_str_t cmd = NM_INIT_STR;

    nm_str_copy(&vm_dir, &nm_cfg_get()->vm_dir);
    nm_str_add_char(&vm_dir, '/');
    nm_str_add_str(&vm_dir, name);

    if (mkdir(vm_dir.data, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0)
    {
        nm_bug(_("%s: cannot create VM directory %s: %s"),
               __func__, vm_dir.data, strerror(errno));
    }

    for (size_t n = 0; n < drives->n_memb; n++)
    {
        nm_str_format(&cmd, "%s %s/%s %s/%s",
                      NM_STRING(NM_USR_PREFIX) NM_QEMU_CONVERT,
                      templ_path, (nm_drive_file(drives->data[n])).data,
                      vm_dir.data, (nm_drive_file(drives->data[n])).data);
#ifdef NM_DEBUG
        nm_debug("ova: exec: %s\n", cmd.data);
#endif
        if (nm_spawn_process(&cmd) != NM_OK)
        {
            rmdir(vm_dir.data);
            nm_bug(_("%s: cannot create image file"), __func__);
        }

        nm_str_trunc(&cmd, 0);
    }

    nm_str_free(&vm_dir);
    nm_str_free(&cmd);
}

static void nm_ovf_to_db(nm_vm_t *vm, const nm_vect_t *drives)
{
    uint64_t last_mac;
    uint32_t last_vnc;

    nm_str_alloc_text(&vm->ifs.driver, NM_DEFAULT_NETDRV);

    nm_form_get_last(&last_mac, &last_vnc);
    nm_str_format(&vm->vncp, "%u", last_vnc);

    nm_add_vm_to_db(vm, last_mac, NM_IMPORT_VM, drives);
}

static int nm_ova_get_data(nm_vm_t *vm)
{
    int rc = NM_OK;
    nm_vect_t err = NM_INIT_VECT;

    nm_get_field_buf(fields[NM_OVA_FLD_SRC], &vm->srcp);
    nm_get_field_buf(fields[NM_OVA_FLD_ARCH], &vm->arch);
    if (field_status(fields[NM_OVA_FLD_NAME]))
    {
        nm_get_field_buf(fields[NM_OVA_FLD_NAME], &vm->name);
        nm_form_check_data(_(NM_OVF_FORM_NAME), vm->name, err);
    }

    nm_form_check_data(_(NM_OVF_FORM_PATH), vm->srcp, err);
    nm_form_check_data(_(NM_OVF_FORM_ARCH), vm->arch, err);

    rc = nm_print_empty_fields(&err);

    nm_vect_free(&err, NULL);

    return rc;
}

#endif /* NM_WITH_OVF_SUPPORT */
/* vim:set ts=4 sw=4 fdm=marker: */