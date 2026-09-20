#ifndef SCOS_EFI_H
#define SCOS_EFI_H
/* Minimal UEFI 2.x protocol declarations, not a firmware implementation.
 * x64 firmware calls use the Microsoft ABI; kernel entry remains SysV AMD64. */
#include <stdint.h>
#include <stddef.h>
#define EFIAPI __attribute__((ms_abi))
typedef uint64_t status;
typedef uint16_t char16;
typedef void *handle;
#define ERROR(n) (UINT64_C(0x8000000000000000)|(n))
#define BUFFER_TOO_SMALL ERROR(5)
#define INVALID_PARAMETER ERROR(2)
#define FAILED(s) ((s)>>63)
struct guid {uint32_t a;uint16_t b,c;uint8_t d[8];};
struct header {uint64_t signature;uint32_t revision,size,crc,reserved;};
struct text_output {
 void *reset;
 status(EFIAPI *output)(struct text_output *,const char16 *);
};
struct boot_services {
 struct header header;
 void *raise_tpl,*restore_tpl;
 status(EFIAPI *allocate_pages)(uint32_t,uint32_t,size_t,uint64_t *);
 status(EFIAPI *free_pages)(uint64_t,size_t);
 status(EFIAPI *get_memory_map)(size_t *,void *,size_t *,size_t *,uint32_t *);
 status(EFIAPI *allocate_pool)(uint32_t,size_t,void **);
 status(EFIAPI *free_pool)(void *);
 void *create_event,*set_timer,*wait_event,*signal_event,*close_event,*check_event;
 void *install_protocol,*reinstall_protocol,*uninstall_protocol;
 status(EFIAPI *handle_protocol)(handle,const struct guid *,void **);
 void *reserved,*register_notify,*locate_handle,*locate_path,*install_table;
 void *load_image,*start_image,*exit,*unload_image;
 status(EFIAPI *exit_boot_services)(handle,size_t);
 void *monotonic;
 status(EFIAPI *stall)(size_t);
 status(EFIAPI *watchdog)(size_t,uint64_t,size_t,char16 *);
 void *connect,*disconnect,*open_protocol,*close_protocol,*protocol_info;
 void *protocols_per_handle,*locate_handle_buffer;
 status(EFIAPI *locate_protocol)(const struct guid *,void *,void **);
};
struct config_table {struct guid guid;void *table;};
struct system_table {
 struct header header;
 char16 *vendor;uint32_t revision;
 handle input_handle;void *input;
 handle output_handle;struct text_output *output;
 handle error_handle;struct text_output *error;
 void *runtime;struct boot_services *boot;
 size_t table_count;struct config_table *tables;
};
struct loaded_image {
 uint32_t revision;handle parent;struct system_table *system;handle device;
 void *path,*reserved;uint32_t options_size;void *options,*base;
 uint64_t size;uint32_t code_type,data_type;void *unload;
};
struct file {
 uint64_t revision;
 status(EFIAPI *open)(struct file *,struct file **,const char16 *,uint64_t,uint64_t);
 status(EFIAPI *close)(struct file *);void *delete;
 status(EFIAPI *read)(struct file *,size_t *,void *);void *write;
 status(EFIAPI *get_position)(struct file *,uint64_t *);
 status(EFIAPI *set_position)(struct file *,uint64_t);
};
struct filesystem {uint64_t revision;status(EFIAPI *open_volume)(struct filesystem *,struct file **);};
struct mode_info {uint32_t version,width,height,format,red,green,blue,reserved,stride;};
struct gop_mode {uint32_t max_mode,mode;struct mode_info *info;size_t info_size;uint64_t base;size_t size;};
struct gop {
 status(EFIAPI *query)(struct gop *,uint32_t,size_t *,struct mode_info **);
 status(EFIAPI *set)(struct gop *,uint32_t);void *blt;struct gop_mode *mode;
};
_Static_assert(offsetof(struct boot_services,exit_boot_services)==232,"ExitBootServices ABI");
_Static_assert(offsetof(struct boot_services,locate_protocol)==320,"LocateProtocol ABI");
_Static_assert(offsetof(struct system_table,boot)==96,"system table ABI");
_Static_assert(offsetof(struct loaded_image,device)==24,"loaded image ABI");
_Static_assert(offsetof(struct file,read)==32,"file ABI");
_Static_assert(offsetof(struct gop_mode,base)==24,"GOP ABI");
#endif
