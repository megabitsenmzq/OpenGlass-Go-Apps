#ifndef LKG_MENU_WIFI_CONTROL_H
#define LKG_MENU_WIFI_CONTROL_H

#include "wifi_model.h"

#include <sys/types.h>

#define WIFI_OUTPUT_SIZE 65536

typedef struct {
    pid_t pid;
    int output_fd;
    int status;
    char output[WIFI_OUTPUT_SIZE];
    size_t length;
} WifiProcess;

typedef enum {
    WIFI_OPERATION_CONNECT = 0,
    WIFI_OPERATION_DISCONNECT,
    WIFI_OPERATION_FORGET
} WifiOperation;

void wifi_process_initialize(WifiProcess *process);
void wifi_process_stop(WifiProcess *process);
int wifi_process_poll(WifiProcess *process);
int wifi_control_start_scan(WifiProcess *process);
int wifi_control_start_operation(WifiProcess *process, WifiModel *model,
                                 WifiOperation operation, int saved,
                                 const char *password, size_t password_length);
int wifi_control_start_update(WifiProcess *process, const WifiModel *model);
int wifi_control_update_saved(WifiModel *model);
void wifi_control_update_current(WifiModel *model);

#endif
