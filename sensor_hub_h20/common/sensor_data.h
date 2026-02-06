/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#define SENSOR_DATA_SIZE 35
#define SENSOR_DATA_BATCH_CNT 10

struct sensor_data {
	uint8_t data[SENSOR_DATA_SIZE];
} __packed;

struct sensor_data_batch {
	struct sensor_data data[SENSOR_DATA_BATCH_CNT];
} __packed;

BUILD_ASSERT(sizeof(struct sensor_data) == SENSOR_DATA_SIZE);
BUILD_ASSERT(sizeof(struct sensor_data_batch) ==
	     (SENSOR_DATA_BATCH_CNT * sizeof(struct sensor_data)));

/* Ensure the sensor data batch size does not exceed the IPC buffer size. */
BUILD_ASSERT(sizeof(struct sensor_data_batch) <= CONFIG_PBUF_RX_READ_BUF_SIZE);

#endif /* SENSOR_DATA_H */
