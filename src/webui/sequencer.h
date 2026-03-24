#pragma once
#include <Arduino.h>

struct Sequence {
    float   temperature_c;
    float   speed_mm_s;
    float   duration_s;
};

enum SeqState : uint8_t {
    SEQ_IDLE    = 0,
    SEQ_HEATING = 1,
    SEQ_RUNNING = 2,
    SEQ_NEXT    = 3,
    SEQ_DONE    = 4,
    SEQ_ERROR   = 5,
};

// Sequencer initialisieren und Task starten
void sequencer_init();

// Sequenzen verwalten
bool sequencer_add(float temp_c, float speed_mm_s, float duration_s);
bool sequencer_delete(int index);
void sequencer_clear();
bool sequencer_reorder(const int *order, int count);
int  sequencer_count();
bool sequencer_get(int index, Sequence *s);

// Steuerung
bool sequencer_start(const char *filename = nullptr);
void sequencer_stop();

// Status
SeqState    sequencer_get_state();
int         sequencer_get_active_index();
float       sequencer_get_remaining_s();
const char* sequencer_state_string();
