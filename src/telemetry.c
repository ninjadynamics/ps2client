 #include <stdio.h>
 #include <stdint.h>
 #include <string.h>
 #include <pthread.h>
#ifdef _WIN32
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif
 #include "dctool-telemetry.h"
 #include "telemetry.h"

 static const dctool_telemetry_decoder_v1_t *telemetry_decoder = NULL;
 static pthread_mutex_t telemetry_output_mutex = PTHREAD_MUTEX_INITIALIZER;
 static unsigned int telemetry_malformed = 0;

 ////////////////////////////////
 // BINARY TELEMETRY FUNCTIONS //
 ////////////////////////////////

 int telemetry_load_decoder(const char *path) {
  dctool_telemetry_decoder_get_v1_fn get = NULL;
  const dctool_telemetry_decoder_v1_t *decoder;

#ifdef _WIN32
  HMODULE module = LoadLibraryA(path);
  if (module) { get = (dctool_telemetry_decoder_get_v1_fn)(void (*)(void))GetProcAddress(module, DCTOOL_TELEMETRY_DECODER_ENTRY); }
#else
  void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (module) { *(void **)&get = dlsym(module, DCTOOL_TELEMETRY_DECODER_ENTRY); }
#endif
  if (!module || !get) { fprintf(stderr, "ps2client: telemetry decoder '%s' not loaded; frames will be consumed.\n", path); return -1; }

  decoder = get();
  if (!decoder || decoder->abi_version != DCTOOL_TELEMETRY_DECODER_ABI_V1 || decoder->struct_size < sizeof(*decoder) || !decoder->decode) {
   fprintf(stderr, "ps2client: telemetry decoder '%s' has an incompatible ABI; frames will be consumed.\n", path);
   return -1;
  }
  telemetry_decoder = decoder;
  fprintf(stderr, "ps2client: telemetry decoder: %s\n", decoder->name ? decoder->name : path);
  return 0;
 }

 void telemetry_output(const void *data, size_t size) {
  pthread_mutex_lock(&telemetry_output_mutex);
  fwrite(data, 1, size, stdout);
  pthread_mutex_unlock(&telemetry_output_mutex);
 }

 static void telemetry_emit(void *context, const char *text, uint32_t size) {
  (void)context;
  telemetry_output(text, size);
 }

 void telemetry_receive(const unsigned char *data, int size) {
  uint32_t frame_size;

  // Exact length, header and CRC must agree before any decoder sees it.
  if (size < (int)DCTOOL_TELEMETRY_HEADER_SIZE || size > (int)DCTOOL_TELEMETRY_FRAME_MAX ||
      memcmp(data + DCTOOL_TELEMETRY_OFF_MAGIC, DCTOOL_TELEMETRY_MAGIC, 4) != 0 ||
      data[DCTOOL_TELEMETRY_OFF_VERSION] != DCTOOL_TELEMETRY_WIRE_VERSION ||
      data[DCTOOL_TELEMETRY_OFF_HEADER_SIZE] != DCTOOL_TELEMETRY_HEADER_SIZE ||
      DCTOOL_TELEMETRY_HEADER_SIZE + dctool_telemetry_read_le16(data + DCTOOL_TELEMETRY_OFF_PAYLOAD_SIZE) != (uint32_t)size ||
      dctool_telemetry_read_le32(data + DCTOOL_TELEMETRY_OFF_CRC32) != dctool_telemetry_frame_crc32(data, (uint32_t)size)) {
   if (telemetry_malformed++ % 100 == 0) { fprintf(stderr, "ps2client: dropped malformed telemetry datagram (%u so far).\n", telemetry_malformed); }
   return;
  }

  frame_size = (uint32_t)size;
  if (telemetry_decoder && telemetry_decoder->decode(data, frame_size, telemetry_emit, NULL) == DCTOOL_TELEMETRY_DECODE_ERROR) {
   fprintf(stderr, "ps2client: telemetry decoder rejected frame seq %u.\n", dctool_telemetry_read_le32(data + DCTOOL_TELEMETRY_OFF_SEQUENCE));
  }
 }
