#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const void *buffer, size_t size) {
  const uint8_t *bytes = buffer;
  while (size > 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written <= 0) return -1;
    bytes += written;
    size -= (size_t)written;
  }
  return 0;
}

static void store_le32(uint8_t *target, uint32_t value) {
  target[0] = (uint8_t)value;
  target[1] = (uint8_t)(value >> 8);
  target[2] = (uint8_t)(value >> 16);
  target[3] = (uint8_t)(value >> 24);
}

static int send_request(int fd, uint8_t code, const void *payload, uint32_t size) {
  uint8_t header[5] = {code, 0, 0, 0, 0};
  store_le32(header + 1, size);
  if (write_all(fd, header, sizeof(header)) != 0) return -1;
  return size == 0 || write_all(fd, payload, size) == 0 ? 0 : -1;
}

int main(void) {
  enum { sample_rate = 48000, channels = 2, seconds = 2, chunk_size = 4096 };
  const int frames = sample_rate * seconds;
  int16_t *pcm = calloc((size_t)frames * channels, sizeof(int16_t));
  if (!pcm) {
    fputs("BACHATA_AUDIO_ERROR allocation\n", stderr);
    return 1;
  }

  for (int frame = 0; frame < frames; ++frame) {
    const double t = (double)frame / (double)sample_rate;
    const int16_t sample = (int16_t)(sin(2.0 * M_PI * 440.0 * t) * 12000.0);
    pcm[frame * channels] = sample;
    pcm[frame * channels + 1] = sample;
  }

  const char *socket_path = getenv("BACHATA_ALSA_SOCKET");
  if (!socket_path || socket_path[0] != '/') {
    free(pcm);
    fputs("BACHATA_AUDIO_ERROR socket_path\n", stderr);
    return 2;
  }

  const int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (socket_fd < 0 || strlen(socket_path) >= sizeof(address.sun_path)) {
    if (socket_fd >= 0) close(socket_fd);
    free(pcm);
    fputs("BACHATA_AUDIO_ERROR socket\n", stderr);
    return 3;
  }
  strcpy(address.sun_path, socket_path);
  if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(socket_fd);
    free(pcm);
    fputs("BACHATA_AUDIO_ERROR connect\n", stderr);
    return 4;
  }

  uint8_t prepare[10] = {channels, 1};
  store_le32(prepare + 2, sample_rate);
  store_le32(prepare + 6, chunk_size);
  int status = send_request(socket_fd, 4, prepare, sizeof(prepare));
  const uint8_t *bytes = (const uint8_t *)pcm;
  size_t remaining = (size_t)frames * channels * sizeof(int16_t);
  while (status == 0 && remaining > 0) {
    const uint32_t chunk = remaining > chunk_size ? chunk_size : (uint32_t)remaining;
    status = send_request(socket_fd, 5, bytes, chunk);
    bytes += chunk;
    remaining -= chunk;
  }
  if (status == 0) status = send_request(socket_fd, 6, NULL, 0);
  send_request(socket_fd, 0, NULL, 0);
  close(socket_fd);
  free(pcm);

  if (status != 0) {
    fputs("BACHATA_AUDIO_ERROR write\n", stderr);
    return 5;
  }
  puts("BACHATA_AUDIO_OK underruns=0");
  return 0;
}
