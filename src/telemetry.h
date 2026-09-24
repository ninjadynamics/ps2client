#ifndef __TELEMETRY_H__
#define __TELEMETRY_H__

 #include <stddef.h>

 ////////////////////////////////
 // BINARY TELEMETRY FUNCTIONS //
 ////////////////////////////////

 // Port of the ps2link fork's "pkotlm" push (0x4713 is ps2netfs).
 #define TELEMETRY_PORT 0x4714

 // Loads an optional decoder module (dctool-telemetry.h ABI v1). A missing or
 // incompatible module is reported and ignored: frames are then consumed.
 int telemetry_load_decoder(const char *path);

 // Validates one received datagram as a complete DCTM frame and decodes it.
 // Valid frames without a decoder are consumed silently; malformed datagrams
 // are dropped with a rate-limited notice. Binary never reaches stdout.
 void telemetry_receive(const unsigned char *data, int size);

 // Serialized stdout writes shared with the console thread.
 void telemetry_output(const void *data, size_t size);

#endif
