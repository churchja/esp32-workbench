/*
 * wx_i2c.h -- the one I2C bus on this board, shared.
 *
 * SDA=GPIO3 / SCL=GPIO2 carries three devices: the CST816S touch controller at
 * 0x15, the PCF85063ATL RTC at 0x51 and the BQ25896 PMU at 0x6B. They are on
 * one bus and therefore must share one master handle -- i2c_new_master_bus()
 * on a port that is already open returns ESP_ERR_INVALID_STATE, so a second
 * module creating "its own" bus does not get a private one, it gets nothing.
 *
 * Deliberately NOT in vaultweather.h: that header is the app contract and is
 * included by every translation unit, and there is no reason for the UI or the
 * fetcher to acquire a dependency on the I2C driver.
 */
#pragma once

#include <driver/i2c_master.h>
#include <esp_err.h>

/* Lazily creates the bus on first call; returns the same handle thereafter. */
esp_err_t wx_i2c_bus(i2c_master_bus_handle_t *out);
