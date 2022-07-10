#include <tudor/dbus-launcher.h>
#include "data.h"
#include "ipc.h"

#define RECORD_VARIANT_TYPE "(@ayy@ay)"

bool get_record_guid_finger(GVariant *var, RECGUID *guid, enum tudor_finger *finger) {
    if(!g_variant_check_format_string(var, RECORD_VARIANT_TYPE, FALSE)) return false;

    //Parse the top level variant
    *finger = 0;
    GVariant *guid_var;
    g_variant_get(var, RECORD_VARIANT_TYPE, NULL, finger, &guid_var);

    //Parse the GUID variant
    size_t var_guid_sz;
    const RECGUID *var_guid = (const RECGUID*) g_variant_get_fixed_array(guid_var, &var_guid_sz, 1);
    if(var_guid_sz == sizeof(RECGUID)) *guid = *var_guid;

    g_variant_unref(guid_var);
    return var_guid_sz == sizeof(RECGUID);
}

bool get_print_guid_finger(FpiDeviceTudor *dev, FpPrint *print, RECGUID *guid, enum tudor_finger *finger) {
    //Check the driver ID
    gchar *print_driver;
    g_object_get(print, "driver", &print_driver, NULL);
    if(g_strcmp0(print_driver, fp_device_get_driver(FP_DEVICE(dev))) != 0) {
        g_free(print_driver);
        return false;
    }
    g_free(print_driver);

    //Check if the print is marked as "device stored"
    if(!fp_print_get_device_stored(print)) return false;

    //Get the print data
    GVariant *print_data;
    g_object_get(print, "fpi-data", &print_data, NULL);

    //Peek the GUID and finger
    bool data_valid = get_record_guid_finger(print_data, guid, finger);

    g_variant_unref(print_data);
    return data_valid;
}

bool load_pdata(FpiDeviceTudor *tdev, GByteArray **pdata, GError **error) {
    //Tell the host launcher service to load the pairing data
    GVariant *rets = g_dbus_connection_call_sync(tdev->dbus_con,
        TUDOR_HOST_LAUNCHER_SERVICE, TUDOR_HOST_LAUNCHER_OBJ, TUDOR_HOST_LAUNCHER_INTERF,
        TUDOR_HOST_LAUNCHER_LOAD_PAIRING_DATA_METHOD, g_variant_new("(s)", tdev->sensor_name), G_VARIANT_TYPE("(bay)"), G_DBUS_CALL_FLAGS_NONE,
        G_MAXINT, NULL, error
    );
    if(!rets) return false;

    //Parse return value
    gboolean has_pdata;
    GVariant *pdata_var;
    g_variant_get(rets, "(b@ay)", &has_pdata, &pdata_var);
    g_variant_unref(rets);

    if(has_pdata) {
        //Create pairing data array
        gsize pdata_len;
        const void *pdata_data = g_variant_get_fixed_array(pdata_var, &pdata_len, 1);
        *pdata = g_byte_array_new_take(g_memdup2(pdata_data, pdata_len), pdata_len);

        g_info("Loaded pairing data for tudor sensor '%s' - %lu bytes", tdev->sensor_name, pdata_len);
    } else {
        *pdata = NULL;

        g_info("Loaded pairing data for tudor sensor '%s' - no stored data", tdev->sensor_name);
    }

    g_variant_unref(pdata_var);
    return true;
}

bool store_pdata(FpiDeviceTudor *tdev, GByteArray *pdata, GError **error) {
    //Tell the host launcher service to store the pairing data
    GVariant *params = g_variant_new("(s@ay)", tdev->sensor_name, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pdata->data, pdata->len, 1));
    GVariant *rets = g_dbus_connection_call_sync(tdev->dbus_con,
        TUDOR_HOST_LAUNCHER_SERVICE, TUDOR_HOST_LAUNCHER_OBJ, TUDOR_HOST_LAUNCHER_INTERF,
        TUDOR_HOST_LAUNCHER_STORE_PAIRING_DATA_METHOD, params, NULL, G_DBUS_CALL_FLAGS_NONE,
        G_MAXINT, NULL, error
    );
    if(!rets) return false;
    g_variant_unref(rets);

    g_info("Stored pairing data for tudor sensor '%s' - %u bytes", tdev->sensor_name, pdata->len);
    return true;
}

struct delete_params {
    RECGUID guid;
    enum tudor_finger finger;
};

static void delete_acked_cb(GObject *src_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    FpiDeviceTudor *tdev = FPI_DEVICE_TUDOR(src_obj);
    FpDevice *dev = FP_DEVICE(tdev);
    struct delete_params *params = (struct delete_params*) user_data;

    //Check for error
    GError *error = NULL;
    IPCMessageBuf *msg = g_task_propagate_pointer(task, &error);
    if(!msg) {
        fpi_device_clear_storage_complete(dev, error);
        g_slice_free(struct delete_params, params);
        return;
    }
    ipc_msg_buf_free(msg);

    //Remove the record if it is in the DB
    for(int i = 0; i < tdev->db_records->len; i++) {
        struct record *rec = (struct record*) tdev->db_records->pdata[i];
        if(memcmp(&rec->guid, &params->guid, sizeof(RECGUID)) == 0 && rec->finger == params->finger) {
            g_ptr_array_remove_index_fast(tdev->db_records, i);
            break;
        }
    }

    //Notify libfprint of completion
    fpi_device_delete_complete(dev, NULL);

    g_slice_free(struct delete_params, params);
}

void fpi_device_tudor_delete(FpDevice *dev) {
    FpiDeviceTudor *tdev = FPI_DEVICE_TUDOR(dev);

    //Check if host process is dead
    GError *error = NULL;
    if(check_host_proc_dead(tdev, &error)) {
        fpi_device_delete_complete(dev, error);
        return;
    }

    //Get the print and its GUID / finger
    FpPrint *print;
    fpi_device_get_delete_data(dev, &print);

    struct delete_params *params = g_slice_new(struct delete_params);
    if(!get_print_guid_finger(tdev, print, &params->guid, &params->finger)) {
        fpi_device_delete_complete(dev, fpi_device_error_new(FP_DEVICE_ERROR_DATA_INVALID));
        g_slice_free(struct delete_params, params);
        return;
    }

    //Tell the driver host
    tdev->send_msg->size = sizeof(struct ipc_msg_del_record);
    tdev->send_msg->del_record = (struct ipc_msg_del_record) {
        .type = IPC_MSG_DEL_RECORD,
        .guid = params->guid,
        .finger = params->finger
    };
    send_acked_ipc_msg(tdev, tdev->send_msg, delete_acked_cb, params);
}

static void clear_storage_acked_cb(GObject *src_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    FpiDeviceTudor *tdev = FPI_DEVICE_TUDOR(src_obj);
    FpDevice *dev = FP_DEVICE(tdev);

    //Check for error
    GError *error = NULL;
    IPCMessageBuf *msg = g_task_propagate_pointer(task, &error);
    if(!msg) {
        fpi_device_clear_storage_complete(dev, error);
        return;
    }
    ipc_msg_buf_free(msg);

    //Clear DB records array
    g_ptr_array_set_size(tdev->db_records, 0);

    //Notify libfprint of completion
    fpi_device_clear_storage_complete(dev, NULL);
}

void fpi_device_clear_storage(FpDevice *dev) {
    FpiDeviceTudor *tdev = FPI_DEVICE_TUDOR(dev);

    //Check if host process is dead
    GError *error = NULL;
    if(check_host_proc_dead(tdev, &error)) {
        fpi_device_clear_storage_complete(dev, error);
        return;
    }

    //Tell the driver host
    tdev->send_msg->size = sizeof(enum ipc_msg_type);
    tdev->send_msg->type = IPC_MSG_CLEAR_RECORDS;
    send_acked_ipc_msg(tdev, tdev->send_msg, clear_storage_acked_cb, NULL);
}