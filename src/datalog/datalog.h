#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "freertos/semphr.h"

enum DatalogState : uint8_t {
    DATALOG_IDLE      = 0,
    DATALOG_RECORDING = 1,
    DATALOG_PAUSED    = 2,
    DATALOG_ERROR     = 3,
    DATALOG_STOPPING  = 4,
};

struct DatalogFileInfo {
    char   name[64];
    size_t size_bytes;
    char   date_str[20];
};

struct DatalogSDInfo {
    uint64_t total_bytes;
    uint64_t free_bytes;
    bool     mounted;
};

// Modul initialisieren (SD, Tasks)
bool datalog_init(SPIClass &spi, SemaphoreHandle_t spi_mutex);

// SD-Karte manuell mounten (blockiert kurz)
bool datalog_mount_sd();

// Aufzeichnung steuern
void datalog_set_preamble(const char *text);
bool datalog_start(uint32_t interval_ms = 0, const char *filename = nullptr);
bool datalog_stop();
bool datalog_pause();
bool datalog_resume();
bool datalog_set_interval(uint32_t interval_ms);

// Status
DatalogState datalog_get_state();
const char*  datalog_get_filename();

// Dateiverwaltung
int  datalog_list_files(DatalogFileInfo *files, int max_files);
bool datalog_delete_file(const char *name);
bool datalog_delete_all();
bool datalog_get_sd_info(DatalogSDInfo *info);

// Chunked-Read für HTTP-Download (mutex-aware)
bool datalog_read_chunk(const char *name, size_t offset,
                        uint8_t *buf, size_t buf_size, size_t *bytes_read);
