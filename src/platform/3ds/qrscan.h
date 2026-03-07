#ifndef QRSCAN_H
#define QRSCAN_H

#include <mgba-util/gui/font.h>

struct mGUIRunner;

/* Initialize camera + QR decoder. Returns 0 on success. */
int qrscanInit(void);

/* Shut down camera + QR decoder. */
void qrscanExit(void);

/* Returns true if the scanner is initialized and ready. */
int qrscanIsReady(void);

/* Start scanning (activates camera). */
void qrscanStart(void);

/* Stop scanning (deactivates camera). */
void qrscanStop(void);

/* Returns true if currently scanning. */
int qrscanIsActive(void);

/* Poll for camera frames and decode QR codes. Call once per frame
 * while scanning is active. Returns 1 if a new QR code was decoded. */
int qrscanPoll(void);

/* Get the last decoded QR payload. Returns payload length, 0 if none.
 * Data is copied into buf (up to bufSize). */
int qrscanGetData(uint8_t* buf, int bufSize);

/* Draw scanner UI (camera preview + status) on the overlay screen. */
void qrscanDraw(struct GUIFont* font, int screenW, int screenH);

/* Parse the last decoded QR payload and apply rewards to GBA memory.
 * Returns 1 on success, 0 on invalid/unsupported payload.
 * Sets statusMsg with result description. */
int qrscanApplyReward(struct mGUIRunner* runner);

/* Get the current status message (for display after reward). */
const char* qrscanGetStatus(void);

#endif
