
 #include <stdio.h>
 #include <stdlib.h>
 #include <fcntl.h>
 #include <string.h>
 #include <dirent.h>
 #include <unistd.h>
 #include <time.h>
 #include <sys/stat.h>
 #include <pthread.h>
#ifndef _WIN32
 #include <netinet/in.h>
 #include <netinet/tcp.h>
#else
 #include <winsock2.h>
 #include <windows.h>
 #define sleep(x) Sleep(x * 1000)
 #define pause() while (1) { Sleep(600000); }
#endif

 #include "network.h"
 #include "ps2link.h"
 #include "utility.h"
 #include "telemetry.h"

 int console_socket = -1;
 int request_socket = -1;
 int command_socket = -1;
 int telemetry_socket = -1;

 pthread_t console_thread_id;
 pthread_t request_thread_id;
 static int request_thread_started = 0;
 pthread_t telemetry_thread_id;

 int ps2link_counter = 0;

 // Readiness and delivery bounds. The fileio listener comes up while a reset
 // ps2link reloads its IOP modules; UDP commands are retransmitted at the
 // retry interval until acknowledged (fork) or proven by fileio (stock).
 #define PS2LINK_RETRY_MS 250
 #define PS2LINK_CONNECT_DEADLINE_MS 60000
 // Commands that travel on the UDP command port alone (reset) only glance at
 // fileio: a lost earlier reset can leave ps2link running without it.
 #define PS2LINK_OPTIONAL_CONNECT_MS 3000
 #define PS2LINK_EXECEE_DEADLINE_MS 15000
 #define PS2LINK_HANDSHAKE_DEADLINE_MS 3000
 // A reset restarts ps2link's network stack; a switch port that renegotiates
 // can take 30 s or more to forward again, so readiness gets a long bound.
 #define PS2LINK_RESET_DEADLINE_MS 60000
 // An old generation still answering this long after an acknowledged reset
 // never received it on the EE; the reset is sent again.
 #define PS2LINK_RESET_RESEND_MS 5000

 // The first fileio request proves that ps2link executed the EXECEE command.
 // While that proof is pending, a closed fileio stream belongs to the ps2link
 // instance being reset, so the request thread reconnects to its successor.
 pthread_mutex_t ps2link_request_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t ps2link_request_cond = PTHREAD_COND_INITIALIZER;
 int ps2link_request_seen = 0;
 // Fileio requests served so far; EXECEE2 waits while this advances.
 unsigned long ps2link_request_count = 0;
 int ps2link_execee_pending = 0;

 static char ps2link_hostname[256];

 // Wall-clock milliseconds: the same clock pthread_cond_timedwait uses.
 static long long ps2link_now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
 }

 static struct timespec ps2link_ms_to_timespec(long long ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000;
  return ts;
 }

 // ps2link_dd is now an array of structs
 struct {
    char *pathname; // remember to free when closing dir
    DIR *dir;
 } ps2link_dd[10] = {
  { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL },
  { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }
 };

 ///////////////////////
 // PS2LINK FUNCTIONS //
 ///////////////////////

 static int ps2link_connect_request_within(int deadline_ms) {
  long long start = ps2link_now_ms();
  long long waited = 0;
  int one = 1;

  // A refusal means ps2link is not listening yet (for example while a reset
  // reloads it), so retry until the deadline.
  while ((request_socket = network_connect(ps2link_hostname, 0x4711, SOCK_STREAM)) == -2) {
   waited = ps2link_now_ms() - start;
   if (waited >= deadline_ms) { break; }
   usleep(PS2LINK_RETRY_MS * 1000);
  }
  if (request_socket < 0) { fprintf(stderr, "Error: Could not connect to the ps2link fileio port 0x4711 within %d ms.\n", deadline_ms); return -1; }
  if (waited > 0) { fprintf(stderr, "ps2client: fileio connected after %d ms of retries.\n", (int)waited); }

  // Disable Nagle algorithm: without TCP_NODELAY, the two-part response
  // (small header then data) interacts with PS2's delayed-ACK and causes
  // ~200 ms stall per read() syscall, making fread() ~400x slower than read().
  setsockopt(request_socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
  return 0;
 }

 static int ps2link_connect_request(void) {
  return ps2link_connect_request_within(PS2LINK_CONNECT_DEADLINE_MS);
 }

 void ps2link_expect_execee(void) {

  // Set before connecting: the reset ps2link can close the stream first.
  pthread_mutex_lock(&ps2link_request_mutex);
  ps2link_execee_pending = 1;
  pthread_mutex_unlock(&ps2link_request_mutex);

 }

 int ps2link_connect(char *hostname, int fileio_optional) {

  strncpy(ps2link_hostname, hostname, sizeof(ps2link_hostname) - 1);

  // Connect to the console port. A bind failure means another receiver owns it.
  console_socket = network_listen(0x4712, SOCK_DGRAM);
  if (console_socket < 0) { fprintf(stderr, "Error: Could not bind the console port 0x4712; is another ps2client running?\n"); return -1; }

  // Give console bursts room while terminal output catches up.
  { int size = 1 << 20; setsockopt(console_socket, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size)); }

  // Create the console thread.
  pthread_create(&console_thread_id, NULL, ps2link_thread_console, (void *)&console_thread_id);

  // Binary telemetry is optional: a busy port only loses frames, not the console.
  telemetry_socket = network_listen(TELEMETRY_PORT, SOCK_DGRAM);
  if (telemetry_socket < 0) { fprintf(stderr, "ps2client: could not bind the telemetry port 0x%x; frames will be lost.\n", TELEMETRY_PORT); }
  else {
   int size = 1 << 20;
   setsockopt(telemetry_socket, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size));
   pthread_create(&telemetry_thread_id, NULL, ps2link_thread_telemetry, (void *)&telemetry_thread_id);
  }

  // Connect to the request port. Without it, only command-port work proceeds.
  if (fileio_optional) {
   if (ps2link_connect_request_within(PS2LINK_OPTIONAL_CONNECT_MS) < 0) {
    fprintf(stderr, "ps2client: continuing without fileio; ps2link commands use the command port.\n");
   }
  } else if (ps2link_connect_request() < 0) { return -1; }

  // Create the request thread.
  if (request_socket >= 0) {
   pthread_create(&request_thread_id, NULL, ps2link_thread_request, (void *)&request_thread_id);
   request_thread_started = 1;
  }

  // Connect to the command port.
  command_socket = network_connect(hostname, 0x4712, SOCK_DGRAM);
  if (command_socket < 0) { fprintf(stderr, "Error: Could not open the ps2link command port 0x4712.\n"); return -1; }

  // Legacy settling delay for the unacknowledged commands. EXECEE does not
  // rely on it: it retransmits until ps2link's first fileio request arrives.
  sleep(1);

  // End function.
  return 0;

 }

 int ps2link_mainloop(int timeout) {

  // Disconnect from the command port.
  if (network_disconnect(command_socket) < 0) { return -1; }

  // If no timeout was given, timeout immediately.
  if (timeout == 0) { return 0; }

  // If timeout was never, wait forever.
  if (timeout < 0) { pause(); }

  // Increment the timeout counter until timeout is reached.
  while (ps2link_counter++ < timeout) { sleep(1); };

  // End function.
  return 0;

 }

 int ps2link_disconnect(void) {
  // Kill created threads.
  if (request_thread_started) { pthread_cancel(request_thread_id); }
  pthread_cancel(console_thread_id);
  if (telemetry_socket >= 0) { pthread_cancel(telemetry_thread_id); network_disconnect(telemetry_socket); }
  
  // Disconnect from the command port.
  if (network_disconnect(command_socket) < 0) { return -1; }

  // Disconnect from the request port.
  if (request_socket >= 0 && network_disconnect(request_socket) < 0) { return -1; }

  // Disconnect from console port.
  if (network_disconnect(console_socket) < 0) { return -1; }

  // End function.
  return 0;

 }

 ///////////////////////////////
 // PS2LINK COMMAND FUNCTIONS //
 ///////////////////////////////

 typedef struct { unsigned int number; unsigned short length; unsigned int protocol; unsigned int marker; unsigned int features; unsigned int generation; unsigned int ee_ready; } PACKED ps2link_version_t;

 // Send a request until a reply of the expected command and size arrives,
 // retransmitting every retry interval until the deadline. match_offset > 0
 // also requires the u32 at that offset to equal match_value. Unmatched
 // datagrams (stale replies to earlier retransmissions) are drained. Returns
 // 0 with the reply copied, or -1 at the deadline.
 // Listen (without sending) until a matching reply arrives or `until` passes.
 // Unmatched datagrams are drained. Returns 0 with the reply copied, or -1.
 static int ps2link_await(unsigned int reply_number, void *reply, int reply_size, int match_offset, unsigned int match_value, long long until) {
  char buffer[512];

  for (;;) {
   long long wait = until - ps2link_now_ms();
   struct timeval tv;
   fd_set fds;
   int size;

   if (wait <= 0) { return -1; }
   FD_ZERO(&fds); FD_SET(command_socket, &fds);
   tv.tv_sec = (long)(wait / 1000); tv.tv_usec = (long)(wait % 1000) * 1000;
   if (select(command_socket + 1, &fds, NULL, NULL, &tv) <= 0) { return -1; }

   // A refused datagram (ps2link restarting) surfaces here as an error.
   size = recv(command_socket, buffer, sizeof(buffer), 0);
   if (size != reply_size || ntohl(*(unsigned int *)buffer) != reply_number) { continue; }
   if (match_offset > 0) { unsigned int value; memcpy(&value, buffer + match_offset, sizeof(value)); if (ntohl(value) != match_value) { continue; } }
   memcpy(reply, buffer, reply_size);
   return 0;
  }
 }

 static int ps2link_transact(const void *request, int request_size, unsigned int reply_number, void *reply, int reply_size, int match_offset, unsigned int match_value, int deadline_ms, int *attempts) {
  long long deadline = ps2link_now_ms() + deadline_ms;

  *attempts = 0;
  while (ps2link_now_ms() < deadline) {
   long long retry = ps2link_now_ms() + PS2LINK_RETRY_MS;

   ++*attempts;
   network_send(command_socket, (void *)request, request_size);
   if (ps2link_await(reply_number, reply, reply_size, match_offset, match_value, retry < deadline ? retry : deadline) == 0) { return 0; }
  }
  return -1;
 }

 static int ps2link_version(ps2link_version_t *version, int deadline_ms) {
  struct { unsigned int number; unsigned short length; } PACKED command;
  int attempts;

  command.number = htonl(PS2LINK_COMMAND_VERSION);
  command.length = htons(sizeof(command));
  if (ps2link_transact(&command, sizeof(command), PS2LINK_REPLY_VERSION, version, sizeof(*version), 0, 0, deadline_ms, &attempts) < 0) { return -1; }
  version->protocol = ntohl(version->protocol);
  version->marker = ntohl(version->marker);
  version->features = ntohl(version->features);
  version->generation = ntohl(version->generation);
  version->ee_ready = ntohl(version->ee_ready);
  return 0;
 }

 // A fork ps2link proves a reset by answering with a new boot generation
 // once its EE command handler is ready. Stock ps2link gets the legacy send.
 int ps2link_command_reset(void) {
  struct { unsigned int number; unsigned short length; } PACKED command;
  struct { unsigned int number; unsigned short length; unsigned int generation; } PACKED command2;
  struct { unsigned int number; unsigned short length; unsigned int generation; unsigned int accepted; } PACKED reply2;
  ps2link_version_t version;
  long long deadline;
  long long resend;
  unsigned int generation;
  int attempts;

  if (ps2link_version(&version, PS2LINK_HANDSHAKE_DEADLINE_MS) < 0 || !(version.features & PS2LINK_FEATURE_RESET2)) {
   fprintf(stderr, "ps2client: no fork VERSION reply; sending legacy reset (unconfirmed).\n");
   command.number = htonl(PS2LINK_COMMAND_RESET);
   command.length = htons(sizeof(command));
   return network_send(command_socket, &command, sizeof(command));
  }

  generation = version.generation;
  command2.number = htonl(PS2LINK_COMMAND_RESET2);
  command2.length = htons(sizeof(command2));
  command2.generation = htonl(generation);
  if (ps2link_transact(&command2, sizeof(command2), PS2LINK_REPLY_RESET2, &reply2, sizeof(reply2), 0, 0, PS2LINK_HANDSHAKE_DEADLINE_MS, &attempts) < 0) {
   fprintf(stderr, "Error: ps2link P%u (boot %u) did not acknowledge reset.\n", version.marker, generation);
   return -1;
  }

  // The restarted ps2link reloads its IOP modules before it can answer. An
  // IOP that still answers with the old generation after the resend interval
  // acknowledged the reset but the EE never acted on it (the IOP-to-EE
  // command can be lost while a program runs): send it again. A repeated
  // reset only repeats the IOP's unmount and its command to the EE.
  deadline = ps2link_now_ms() + PS2LINK_RESET_DEADLINE_MS;
  resend = ps2link_now_ms() + PS2LINK_RESET_RESEND_MS;
  while (ps2link_now_ms() < deadline) {
   if (ps2link_version(&version, (int)(deadline - ps2link_now_ms())) < 0) { break; }
   if (version.generation != generation && version.ee_ready) {
    fprintf(stderr, "ps2client: reset confirmed: ps2link P%u boot %u -> %u.\n", version.marker, generation, version.generation);
    return 0;
   }
   if (version.generation == generation && ps2link_now_ms() >= resend) {
    fprintf(stderr, "ps2client: ps2link boot %u still running; resending reset.\n", generation);
    if (ps2link_transact(&command2, sizeof(command2), PS2LINK_REPLY_RESET2, &reply2, sizeof(reply2), 0, 0, PS2LINK_HANDSHAKE_DEADLINE_MS, &attempts) == 0 &&
        !ntohl(reply2.accepted) && ntohl(reply2.generation) != generation) {
     // The successor answered: the earlier reset took effect meanwhile.
     continue;
    }
    resend = ps2link_now_ms() + PS2LINK_RESET_RESEND_MS;
   }
   usleep(PS2LINK_RETRY_MS * 1000);
  }
  fprintf(stderr, "Error: ps2link boot %u did not come back ready within %d ms of reset.\n", generation, PS2LINK_RESET_DEADLINE_MS);
  return -1;

 }

 int ps2link_command_execiop(int argc, char **argv) {
  struct { unsigned int number; unsigned short length; int argc; char argv[256]; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_EXECIOP);
  command.length = htons(sizeof(command));
  command.argc   = htonl(argc);
  fix_argv(command.argv, argv);

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_execee(int argc, char **argv) {
  struct { unsigned int number; unsigned short length; int argc; char argv[256]; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_EXECEE);
  command.length = htons(sizeof(command));
  command.argc   = htonl(argc);
  fix_argv(command.argv, argv);

  // A fork ps2link acknowledges EXECEE2 with the EE's decision. It accepts
  // the command only once its EE handler is ready, so wait for that first;
  // the ID lets it answer retransmissions without executing twice.
  {
   struct { unsigned int number; unsigned short length; unsigned int id; int argc; char argv[256]; } PACKED command2;
   struct { unsigned int number; unsigned short length; unsigned int id; int status; } PACKED reply2;
   ps2link_version_t version;
   long long deadline = ps2link_now_ms() + PS2LINK_EXECEE_DEADLINE_MS;
   unsigned int id;
   int attempts;
   int status;

   if (ps2link_version(&version, PS2LINK_HANDSHAKE_DEADLINE_MS) == 0 && (version.features & PS2LINK_FEATURE_EXECEE2)) {
    while (!version.ee_ready) {
     if (ps2link_now_ms() >= deadline) { fprintf(stderr, "Error: ps2link P%u boot %u never became ready.\n", version.marker, version.generation); return -1; }
     usleep(PS2LINK_RETRY_MS * 1000);
     if (ps2link_version(&version, (int)(deadline - ps2link_now_ms())) < 0) { fprintf(stderr, "Error: ps2link stopped answering VERSION.\n"); return -1; }
    }

    id = ((unsigned int)ps2link_now_ms() ^ ((unsigned int)getpid() << 16)) | 1;
    command2.number = htonl(PS2LINK_COMMAND_EXECEE2);
    command2.length = htons(sizeof(command2));
    command2.id = htonl(id);
    command2.argc = command.argc;
    memcpy(command2.argv, command.argv, sizeof(command2.argv));
    // ps2link replies only after the whole ELF has loaded over fileio, and
    // the reply can be lost once the program starts. Two phases:
    //  1. Deliver: retransmit until ps2link answers or its first fileio
    //     request proves it has the command.
    //  2. Listen without sending while the load progresses. Retransmitting
    //     into a starting program floods ps2link's command port and has
    //     killed its network (fileio closed, console silent, no reset).
    // A load that ran but was never acknowledged still started the program:
    // stay attached (detaching strands it without fileio).
    {
     unsigned long start, seen;
     long long retry = 0;
     int acknowledged = 0;

     pthread_mutex_lock(&ps2link_request_mutex);
     start = ps2link_request_count;
     pthread_mutex_unlock(&ps2link_request_mutex);
     seen = start;
     attempts = 0;
     for (;;) {
      long long now = ps2link_now_ms();
      if (now >= deadline) { break; }
      pthread_mutex_lock(&ps2link_request_mutex);
      seen = ps2link_request_count;
      pthread_mutex_unlock(&ps2link_request_mutex);
      if (seen != start) { break; }
      if (now >= retry) {
       ++attempts;
       network_send(command_socket, &command2, sizeof(command2));
       retry = now + PS2LINK_RETRY_MS;
      }
      if (ps2link_await(PS2LINK_REPLY_EXECEE2, &reply2, sizeof(reply2), 6, id, retry < deadline ? retry : deadline) == 0) { acknowledged = 1; break; }
     }
     if (!acknowledged && seen == start) {
      pthread_mutex_lock(&ps2link_request_mutex);
      ps2link_execee_pending = 0;
      pthread_mutex_unlock(&ps2link_request_mutex);
      fprintf(stderr, "Error: ps2link P%u boot %u did not answer or load execee after %d attempts in %d ms.\n", version.marker, version.generation, attempts, PS2LINK_EXECEE_DEADLINE_MS);
      return -1;
     }
     // Phase 2: silent. Each new fileio request re-arms the progress deadline.
     while (!acknowledged) {
      unsigned long before = seen;
      if (ps2link_await(PS2LINK_REPLY_EXECEE2, &reply2, sizeof(reply2), 6, id, ps2link_now_ms() + PS2LINK_EXECEE_DEADLINE_MS) == 0) { acknowledged = 1; break; }
      pthread_mutex_lock(&ps2link_request_mutex);
      seen = ps2link_request_count;
      pthread_mutex_unlock(&ps2link_request_mutex);
      if (seen == before) { break; }
     }
     // The load has gone quiet. ps2link answers a retransmission from the
     // result it cached for this ID, so ask a few more times, spaced out.
     for (int ask = 0; !acknowledged && ask < 3; ask++) {
      ++attempts;
      network_send(command_socket, &command2, sizeof(command2));
      if (ps2link_await(PS2LINK_REPLY_EXECEE2, &reply2, sizeof(reply2), 6, id, ps2link_now_ms() + PS2LINK_RETRY_MS) == 0) { acknowledged = 1; }
     }
     if (!acknowledged) {
      pthread_mutex_lock(&ps2link_request_mutex);
      ps2link_execee_pending = 0;
      pthread_mutex_unlock(&ps2link_request_mutex);
      fprintf(stderr, "ps2client: ps2link P%u boot %u loaded the ELF but never acknowledged execee (%d sends); staying attached.\n", version.marker, version.generation, attempts);
      return 0;
     }
    }

    pthread_mutex_lock(&ps2link_request_mutex);
    ps2link_execee_pending = 0;
    pthread_mutex_unlock(&ps2link_request_mutex);

    status = ntohl(reply2.status);
    if (status == PS2LINK_EXEC_STARTED) {
     fprintf(stderr, "ps2client: execee started on ps2link P%u boot %u%s.\n", version.marker, version.generation, attempts > 1 ? " (retransmitted)" : "");
     return 0;
    }
    if (status == PS2LINK_EXEC_BUSY) { fprintf(stderr, "Error: ps2link rejected execee: a program is already running; reset first.\n"); }
    else if (status == PS2LINK_EXEC_LOAD_FAILED) { fprintf(stderr, "Error: ps2link could not load the ELF.\n"); }
    else { fprintf(stderr, "Error: ps2link could not start the ELF thread (status %d).\n", status); }
    return -1;
   }
   fprintf(stderr, "ps2client: no fork VERSION reply; using legacy execee.\n");
  }

  // EXECEE has no reply, and a datagram sent before a restarted ps2link can
  // receive it is lost (the target stays on its welcome screen). Loading the
  // ELF starts with a fileio request, so retransmit the identical command
  // until one arrives. ps2link runs EE commands in order, so a duplicate
  // queued behind the load finds the user thread and is rejected. A request
  // from an already running program also satisfies this wait; that program
  // rejects EXECEE as before.
  long long deadline = ps2link_now_ms() + PS2LINK_EXECEE_DEADLINE_MS;
  int attempts = 0;
  int seen;

  pthread_mutex_lock(&ps2link_request_mutex);
  ps2link_request_seen = 0;
  ps2link_execee_pending = 1;
  for (;;) {
   long long now = ps2link_now_ms();
   struct timespec wake;

   if (now >= deadline) { break; }
   attempts++;
   pthread_mutex_unlock(&ps2link_request_mutex);
   if (network_send(command_socket, &command, sizeof(command)) < 0) { fprintf(stderr, "ps2client: execee send %d failed; retrying.\n", attempts); }
   pthread_mutex_lock(&ps2link_request_mutex);

   wake = ps2link_ms_to_timespec(now + PS2LINK_RETRY_MS < deadline ? now + PS2LINK_RETRY_MS : deadline);
   while (!ps2link_request_seen) {
    if (pthread_cond_timedwait(&ps2link_request_cond, &ps2link_request_mutex, &wake) != 0) { break; }
   }
   if (ps2link_request_seen) { break; }
  }
  seen = ps2link_request_seen;
  ps2link_execee_pending = 0;
  pthread_mutex_unlock(&ps2link_request_mutex);

  if (!seen) { fprintf(stderr, "Error: ps2link did not start execee after %d attempts in %d ms.\n", attempts, PS2LINK_EXECEE_DEADLINE_MS); return -1; }
  if (attempts > 1) { fprintf(stderr, "ps2client: execee confirmed after %d attempts.\n", attempts); }
  return 0;

 }

 int ps2link_command_poweroff(void) {
  struct { unsigned int number; unsigned short length; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_POWEROFF);
  command.length = htons(sizeof(command));

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_scrdump(void) {
  struct { unsigned int number; unsigned short length; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_SCRDUMP);
  command.length = htons(sizeof(command));

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_netdump(void) {
  struct { unsigned int number; unsigned short length; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_NETDUMP);
  command.length = htons(sizeof(command));

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_dumpmem(unsigned int offset, unsigned int size, char *pathname) {
  struct { unsigned int number; unsigned short length; unsigned int offset; unsigned int size; char pathname[256]; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_DUMPMEM);
  command.length = htons(sizeof(command));
  command.offset = htonl(offset);
  command.size   = htonl(size);
  if (pathname) { snprintf(command.pathname, sizeof(command.pathname), "%s", pathname); }

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_startvu(int vu) {
  struct { unsigned int number; unsigned short length; int vu; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_STARTVU);
  command.length = htons(sizeof(command));
  command.vu     = htonl(vu);

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_stopvu(int vu) {
  struct { unsigned int number; unsigned short length; int vu; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_STOPVU);
  command.length = htons(sizeof(command));
  command.vu     = htonl(vu);

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_dumpreg(int type, char *pathname) {
  struct { unsigned int number; unsigned short length; int type; char pathname[256]; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_DUMPREG);
  command.length = htons(sizeof(command));
  command.type   = htonl(type);
  if (pathname) { snprintf(command.pathname, sizeof(command.pathname), "%s", pathname); }

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_gsexec(unsigned short size, char *pathname) {
  struct { unsigned int number; unsigned short length; unsigned short size; char pathname[256]; } PACKED command;

  // Build the command packet..
  command.number = htonl(PS2LINK_COMMAND_GSEXEC);
  command.length = htons(sizeof(command));
  command.size   = htonl(size);
  if (pathname) { snprintf(command.pathname, sizeof(command.pathname), "%s", pathname); }

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_writemem(unsigned int offset, unsigned int size, char *pathname) {
  struct { unsigned int number; unsigned short length; unsigned int offset; unsigned int size; char pathname[256]; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_WRITEMEM);
  command.length = htons(sizeof(command));
  command.offset = htonl(offset);
  command.size   = htonl(size);
  if (pathname) { snprintf(command.pathname, sizeof(command.pathname), "%s", pathname); }

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

 }

 int ps2link_command_iopexcep(void) {
  struct { unsigned int number; unsigned short length; } PACKED command;

  // Build the command packet.
  command.number = htonl(PS2LINK_COMMAND_IOPEXCEP);
  command.length = htons(sizeof(command));

  // Send the command packet.
  return network_send(command_socket, &command, sizeof(command));

  // End function.
  return 0;

 }

 ///////////////////////////////
 // PS2LINK REQUEST FUNCTIONS //
 ///////////////////////////////

 int ps2link_request_open(void *packet) {
  struct { unsigned int number; unsigned short length; int flags; char pathname[256]; } PACKED *request = packet;
  int result = -1;
  struct stat stats;

  // Fix the arguments.
  fix_pathname(request->pathname);
  request->flags = fix_flags(ntohl(request->flags));

  if(((stat(request->pathname, &stats) == 0) && (!S_ISDIR(stats.st_mode))) || (request->flags & O_CREAT))
  {
  // Perform the request.
#if defined (__CYGWIN__) || defined (__MINGW32__)
    result = open(request->pathname, request->flags | O_BINARY, 0644);
#else
    result = open(request->pathname, request->flags, 0644);
#endif
  }

  // Send the response.
  return ps2link_response_open(result);

 }

 int ps2link_request_close(void *packet) {
  struct { unsigned int number; unsigned short length; int fd; } PACKED *request = packet;
  int result = -1;

  // Perform the request.
  result = close(ntohl(request->fd));

  // Send the response.
  return ps2link_response_close(result);

 }

 int ps2link_request_read(void *packet) {
  struct { unsigned int number; unsigned short length; int fd; int size; } PACKED *request = packet;
  int result = -1, size = -1; char buffer[65536];

  // If a big read is requested...
  if (ntohl(request->size) > sizeof(buffer)) {

   // Allocate the bigbuffer.
   char *bigbuffer = malloc(ntohl(request->size));

   // Perform the request.
   result = size = read(ntohl(request->fd), bigbuffer, ntohl(request->size));

   // Send the response.
   ps2link_response_read(result, size);

   // Send the response data.
   network_send(request_socket, bigbuffer, size);

   // Free the bigbuffer.
   free(bigbuffer);

  // Else, a normal read is requested...
  } else {

   // Perform the request.
   result = size = read(ntohl(request->fd), buffer, ntohl(request->size));

   // Send the response.
   ps2link_response_read(result, size);

   // Send the response data.
   network_send(request_socket, buffer, size);

  }

  // End function.
  return 0;

 }

 int ps2link_request_write(void *packet) {
  struct { unsigned int number; unsigned short length; int fd; int size; } PACKED *request = packet;
  int result = -1; char buffer[65536];

  // If a big write is requested...
  if (ntohl(request->size) > sizeof(buffer)) {

   // Allocate the bigbuffer.
   char *bigbuffer = malloc(ntohl(request->size));

   // Read the request data.
   network_receive_all(request_socket, bigbuffer, ntohl(request->size));

   // Perform the request.
   result = write(ntohl(request->fd), bigbuffer, ntohl(request->size));

   // Send the response.
   ps2link_response_write(result);

   // Free the bigbuffer.
   free(bigbuffer);

  // Else, a normal write is requested...
  } else {

   // Read the request data.
   network_receive_all(request_socket, buffer, ntohl(request->size));

   // Perform the request.
   result = write(ntohl(request->fd), buffer, ntohl(request->size));

   // Send the response.
   ps2link_response_write(result);

  }

  // End function.
  return 0;

 }

 int ps2link_request_lseek(void *packet) {
  struct { unsigned int number; unsigned short length; int fd, offset, whence; } PACKED *request = packet;
  int result = -1;

  // Perform the request.
  result = lseek(ntohl(request->fd), ntohl(request->offset), ntohl(request->whence));

  // Send the response.
  return ps2link_response_lseek(result);

 }

 int ps2link_request_opendir(void *packet) {
  struct { unsigned int command; unsigned short length; int flags; char pathname[256]; } PACKED *request = packet;
  int result = -1;
  struct stat stats;

  // Fix the arguments.
  fix_pathname(request->pathname);

  if((stat(request->pathname, &stats) == 0) && (S_ISDIR(stats.st_mode)))
  {
      // Allocate an available directory descriptor.
      for (int loop0=0; loop0<10; loop0++) { if (ps2link_dd[loop0].dir == NULL) { result = loop0; break; } }

      // Perform the request.
      if (result != -1)
      {
        ps2link_dd[result].pathname = (char *) malloc(strlen(request->pathname) + 1);
        strcpy(ps2link_dd[result].pathname, request->pathname);
        ps2link_dd[result].dir = opendir(request->pathname);
      }
  }

  // Send the response.
  return ps2link_response_opendir(result);
}

 int ps2link_request_closedir(void *packet) {
  struct { unsigned int number; unsigned short length; int dd; } PACKED *request = packet;
  int result = -1;

  // Perform the request.
  result = closedir(ps2link_dd[ntohl(request->dd)].dir);

  if(ps2link_dd[ntohl(request->dd)].pathname)
  {
    free(ps2link_dd[ntohl(request->dd)].pathname);
    ps2link_dd[ntohl(request->dd)].pathname = NULL;
  }

  // Free the directory descriptor.
  ps2link_dd[ntohl(request->dd)].dir = NULL;

  // Send the response.
  return ps2link_response_closedir(result);

 }

 int ps2link_request_readdir(void *packet) {
    DIR *dir;
  struct { unsigned int number; unsigned short length; int dd; } PACKED *request = packet;
  struct dirent *dirent; struct stat stats; struct tm *loctime;
  unsigned int mode; unsigned char ctime[8]; unsigned char atime[8]; unsigned char mtime[8];
  char tname[512];

    dir = ps2link_dd[ntohl(request->dd)].dir;

  // Perform the request.
  dirent = readdir(dir);

  // If no more entries were found...
  if (dirent == NULL) {

   // Tell the user an entry wasn't found.
   return ps2link_response_readdir(0, 0, 0, 0, NULL, NULL, NULL, 0, NULL);

  }

  // need to specify the directory as well as file name otherwise uses CWD!
  sprintf(tname, "%s/%s", ps2link_dd[ntohl(request->dd)].pathname, dirent->d_name);

  // Fetch the entry's statistics.
  stat(tname, &stats);

  // Convert the mode.
  mode = (stats.st_mode & 0x07);
  if (S_ISDIR(stats.st_mode)) { mode |= 0x20; }
#ifndef _WIN32
  if (S_ISLNK(stats.st_mode)) { mode |= 0x08; }
#endif
  if (S_ISREG(stats.st_mode)) { mode |= 0x10; }

  // Convert the creation time.
  loctime = localtime(&(stats.st_ctime));
  ctime[6] = (unsigned char)loctime->tm_year;
  ctime[5] = (unsigned char)loctime->tm_mon + 1;
  ctime[4] = (unsigned char)loctime->tm_mday;
  ctime[3] = (unsigned char)loctime->tm_hour;
  ctime[2] = (unsigned char)loctime->tm_min;
  ctime[1] = (unsigned char)loctime->tm_sec;

  // Convert the access time.
  loctime = localtime(&(stats.st_atime));
  atime[6] = (unsigned char)loctime->tm_year;
  atime[5] = (unsigned char)loctime->tm_mon + 1;
  atime[4] = (unsigned char)loctime->tm_mday;
  atime[3] = (unsigned char)loctime->tm_hour;
  atime[2] = (unsigned char)loctime->tm_min;
  atime[1] = (unsigned char)loctime->tm_sec;

  // Convert the last modified time.
  loctime = localtime(&(stats.st_mtime));
  mtime[6] = (unsigned char)loctime->tm_year;
  mtime[5] = (unsigned char)loctime->tm_mon + 1;
  mtime[4] = (unsigned char)loctime->tm_mday;
  mtime[3] = (unsigned char)loctime->tm_hour;
  mtime[2] = (unsigned char)loctime->tm_min;
  mtime[1] = (unsigned char)loctime->tm_sec;

  // Send the response.
  return ps2link_response_readdir(1, mode, 0, stats.st_size, ctime, atime, mtime, 0, dirent->d_name);

 }

 int ps2link_request_remove(void *packet) {
  struct { unsigned int number; unsigned short length; char name[256]; } PACKED *request = packet;
  int result = -1;

  // Fix the arguments.
  fix_pathname(request->name);

  // Perform the request.
  result = remove(request->name);

  // Send the response.
  return ps2link_response_remove(result);
 }

 int ps2link_request_mkdir(void *packet) {
  struct { unsigned int number; unsigned short length; int mode; char name[256]; } PACKED *request = packet;
  int result = -1;

  // Fix the arguments.
  fix_pathname(request->name);
  // request->flags = fix_flags(ntohl(request->flags));

  // Perform the request.
  // do we need to use mode in here: request->mode ?

#ifdef _WIN32
  result = mkdir(request->name);
#else
  result = mkdir(request->name, request->mode);
#endif

  // Send the response.
  return ps2link_response_mkdir(result);
 }

 int ps2link_request_rmdir(void *packet) {
  struct { unsigned int number; unsigned short length; char name[256]; } PACKED *request = packet;
  int result = -1;

  // Fix the arguments.
  fix_pathname(request->name);

  // Perform the request.
  result = rmdir(request->name);

  // Send the response.
  return ps2link_response_rmdir(result);
 }

int ps2link_request_getstat(void *packet) {
  struct { unsigned int number; unsigned short length; char name[256]; } PACKED *request = packet;
    struct stat stats; struct tm *loctime;
    int ret;
    unsigned int mode = 0;
    unsigned char ctime[8]; unsigned char atime[8]; unsigned char mtime[8];

    // Fix the arguments.
    fix_pathname(request->name);

    // Fetch the entry's statistics.
    ret = stat(request->name, &stats);

    if (ret == 0) {
        // Convert the mode.
          mode = (stats.st_mode & 0x07);
          if (S_ISDIR(stats.st_mode)) { mode |= 0x20; }
        #ifndef _WIN32
          if (S_ISLNK(stats.st_mode)) { mode |= 0x08; }
        #endif
          if (S_ISREG(stats.st_mode)) { mode |= 0x10; }

          // Convert the creation time.
          loctime = localtime(&(stats.st_ctime));
          ctime[6] = (unsigned char)loctime->tm_year;
          ctime[5] = (unsigned char)loctime->tm_mon + 1;
          ctime[4] = (unsigned char)loctime->tm_mday;
          ctime[3] = (unsigned char)loctime->tm_hour;
          ctime[2] = (unsigned char)loctime->tm_min;
          ctime[1] = (unsigned char)loctime->tm_sec;

          // Convert the access time.
          loctime = localtime(&(stats.st_atime));
          atime[6] = (unsigned char)loctime->tm_year;
          atime[5] = (unsigned char)loctime->tm_mon + 1;
          atime[4] = (unsigned char)loctime->tm_mday;
          atime[3] = (unsigned char)loctime->tm_hour;
          atime[2] = (unsigned char)loctime->tm_min;
          atime[1] = (unsigned char)loctime->tm_sec;

          // Convert the last modified time.
          loctime = localtime(&(stats.st_mtime));
          mtime[6] = (unsigned char)loctime->tm_year;
          mtime[5] = (unsigned char)loctime->tm_mon + 1;
          mtime[4] = (unsigned char)loctime->tm_mday;
          mtime[3] = (unsigned char)loctime->tm_hour;
          mtime[2] = (unsigned char)loctime->tm_min;
          mtime[1] = (unsigned char)loctime->tm_sec;
    }

    return ps2link_response_getstat(ret, mode, 0, stats.st_size, ctime, atime, mtime, 0);
 }

 ////////////////////////////////
 // PS2LINK RESPONSE FUNCTIONS //
 ////////////////////////////////

 int ps2link_response_open(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_OPEN);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_close(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_CLOSE);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_read(int result, int size) {
  struct { unsigned int number; unsigned short length; int result; int size; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_READ);
  response.length = htons(sizeof(response));
  response.result = htonl(result);
  response.size   = htonl(size);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_write(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_WRITE);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_lseek(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_LSEEK);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_opendir(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_OPENDIR);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_closedir(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_CLOSEDIR);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_readdir(int result, unsigned int mode, unsigned int attr, unsigned int size, unsigned char *ctime, unsigned char *atime, unsigned char *mtime, unsigned int hisize, char *name) {
  struct { unsigned int number; unsigned short length; int result; unsigned int mode; unsigned int attr; unsigned int size; unsigned char ctime[8]; unsigned char atime[8]; unsigned char mtime[8]; unsigned int hisize; char name[256]; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_READDIR);
  response.length = htons(sizeof(response));
  response.result = htonl(result);
  response.mode   = htonl(mode);
  response.attr   = htonl(attr);
  response.size   = htonl(size);
  if (ctime) { memcpy(response.ctime, ctime, 8); }
  if (atime) { memcpy(response.atime, atime, 8); }
  if (mtime) { memcpy(response.mtime, mtime, 8); }
  response.hisize = htonl(hisize);
#ifdef _WIN32
  if (name) { sprintf(response.name, "%s", name); }
#else
  if (name) { snprintf(response.name, 256, "%s", name); }
#endif

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));

 }

 int ps2link_response_remove(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_REMOVE);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));
 }

 int ps2link_response_mkdir(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_MKDIR);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));
 }

 int ps2link_response_rmdir(int result) {
  struct { unsigned int number; unsigned short length; int result; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_RMDIR);
  response.length = htons(sizeof(response));
  response.result = htonl(result);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));
 }

int ps2link_response_getstat(int result, unsigned int mode, unsigned int attr, unsigned int size, unsigned char *ctime, unsigned char *atime, unsigned char *mtime, unsigned int hisize) {
  struct { unsigned int number; unsigned short length; int result; unsigned int mode; unsigned int attr; unsigned int size; unsigned char ctime[8]; unsigned char atime[8]; unsigned char mtime[8]; unsigned int hisize; } PACKED response;

  // Build the response packet.
  response.number = htonl(PS2LINK_RESPONSE_GETSTAT);
  response.length = htons(sizeof(response));
  response.result = htonl(result);
  response.mode   = htonl(mode);
  response.attr   = htonl(attr);
  response.size   = htonl(size);
  if (ctime) { memcpy(response.ctime, ctime, 8); }
  if (atime) { memcpy(response.atime, atime, 8); }
  if (mtime) { memcpy(response.mtime, mtime, 8); }
  response.hisize = htonl(hisize);

  // Send the response packet.
  return network_send(request_socket, &response, sizeof(response));
 }

 //////////////////////////////
 // PS2LINK THREAD FUNCTIONS //
 //////////////////////////////

 void *ps2link_thread_console(void *thread_id) {
  char buffer[2048];
  int size;

  // If the socket isn't open, this thread isn't needed.
  if (console_socket < 0) { pthread_exit(thread_id); }

  // Loop forever...
  for (;;) {

   // Wait for network activity.
   network_wait(console_socket, -1);

   // Receive one console datagram; its length, not a terminator, bounds it.
   size = network_receive(console_socket, buffer, sizeof(buffer));
   if (size <= 0) { continue; }

   // Print out the console buffer.
   telemetry_output(buffer, size);

   // Reset the timeout counter.
   ps2link_counter = 0;

  }

  // End function.
  return NULL;

 }

 void *ps2link_thread_telemetry(void *thread_id) {
  unsigned char buffer[2048];
  int size;

  // Loop forever...
  for (;;) {

   // Wait for one telemetry datagram; each carries exactly one frame.
   network_wait(telemetry_socket, -1);
   size = network_receive(telemetry_socket, buffer, sizeof(buffer));
   if (size <= 0) { continue; }
   telemetry_receive(buffer, size);

  }

  // End function.
  return NULL;

 }

 void *ps2link_thread_request(void *thread_id) {
  struct { unsigned int number; unsigned short length; char buffer[512]; } PACKED packet;

  // If the socket isn't open, this thread isn't needed.
  if (request_socket < 0) { pthread_exit(thread_id); }

  // Loop forever...
  for (;;) {

   // Wait for network activity.
   network_wait(request_socket, -1);

   // Read in the request packet header and the rest of the packet. A closed
   // stream means ps2link reset or handed fileio to a newer client.
   if (network_receive_all(request_socket, &packet, 6) < 0 ||
       ntohs(packet.length) < 6 || ntohs(packet.length) > sizeof(packet) ||
       network_receive_all(request_socket, packet.buffer, ntohs(packet.length) - 6) < 0) {
    int pending;

    // Only a pending EXECEE follows the reset ps2link to its successor;
    // otherwise reconnecting would take fileio from a newer client.
    pthread_mutex_lock(&ps2link_request_mutex);
    pending = ps2link_execee_pending && !ps2link_request_seen;
    pthread_mutex_unlock(&ps2link_request_mutex);
    network_disconnect(request_socket);
    if (pending) { fprintf(stderr, "ps2client: fileio connection closed before execee started; reconnecting.\n"); }
    if (!pending || ps2link_connect_request() < 0) { fprintf(stderr, "ps2client: fileio connection closed.\n"); break; }
    continue;
   }

   // Wake an EXECEE waiting for proof that ps2link started loading.
   pthread_mutex_lock(&ps2link_request_mutex);
   ps2link_request_seen = 1;
   ++ps2link_request_count;
   pthread_cond_broadcast(&ps2link_request_cond);
   pthread_mutex_unlock(&ps2link_request_mutex);

   // Perform the requested action.
   if (ntohl(packet.number) == PS2LINK_REQUEST_OPEN)     { ps2link_request_open(&packet);     } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_CLOSE)    { ps2link_request_close(&packet);    } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_READ)     { ps2link_request_read(&packet);     } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_WRITE)    { ps2link_request_write(&packet);    } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_LSEEK)    { ps2link_request_lseek(&packet);    } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_OPENDIR)  { ps2link_request_opendir(&packet);  } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_CLOSEDIR) { ps2link_request_closedir(&packet); } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_READDIR)  { ps2link_request_readdir(&packet);  } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_REMOVE)   { ps2link_request_remove(&packet);   } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_MKDIR)    { ps2link_request_mkdir(&packet);    } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_RMDIR)    { ps2link_request_rmdir(&packet);    } else
   if (ntohl(packet.number) == PS2LINK_REQUEST_GETSTAT)  { ps2link_request_getstat(&packet);  }

   // Reset the timeout counter.
   ps2link_counter = 0;

  }

  // End function.
  return NULL;

 }
