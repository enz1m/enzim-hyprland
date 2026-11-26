#include "DataDevice.hpp"

bool IDataSource::hasDnd() {
    return false;
}

bool IDataSource::dndDone() {
    return false;
}

bool IDataSource::used() {
    return m_wasUsed;
}

void IDataSource::markUsed() {
    m_wasUsed = true;
}

eDataSourceType IDataSource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

void IDataSource::sendDndFinished() {
    ;
}

uint32_t IDataSource::actions() {
    return 7; // all
}

uint32_t IDataSource::preferredDndAction() {
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

void IDataSource::sendDndDropPerformed() {
    ;
}

void IDataSource::sendDndAction(wl_data_device_manager_dnd_action a) {
    ;
}

void IDataOffer::markDead() {
    ;
}
