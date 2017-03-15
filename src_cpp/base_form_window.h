#ifndef BASE_FORM_WINDOW_H_
#define BASE_FORM_WINDOW_H_

#include "window.h"

namespace QManager {

class QMFormWindow : public QMWindow
{  
public:
    QMFormWindow(int height, int width, int starty = 1)
        : QMWindow(height, width, starty) {}
    virtual void Print() = 0;
    virtual ~QMFormWindow() {};

protected:
    void Delete_form();
    void Draw_form();
    void Draw_title(const std::string &msg = _("F10 - cancel, F2 - OK"));
    void Enable_color();
    void Post_form(uint32_t size);
    void ExceptionExit(QMException &err);
    void Gen_mac_address(uint32_t int_count, const std::string &vm_name);

protected:
    std::string sql_query, s_last_mac,
    dbf_, vmdir_, guest_dir, create_guest_dir_cmd, create_img_cmd;
    uint32_t last_vnc, ui_vm_ints;
    uint64_t last_mac;
    VectorString v_last_vnc, v_last_mac, v_name;
    MapString ifaces;
    MapString::iterator itm;
    std::string::iterator its;
    FORM *form;
    std::vector<FIELD*> field;
    int ch, cmd_exit_status;
    MapString *u_dev;
    char **UdevList, **ArchList;
    guest_t guest;
    bool action_ok;
};

} // namespace QManager

#endif
/* vim:set ts=4 sw=4 fdm=marker: */