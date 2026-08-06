import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("Vortek WaitForFences host-waits create-signaled fences without SYNC_FD", () => {
  const server = read(
    "runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer/src/request_handler.c",
  );
  const client = read("runtime/sources/vortek-client/src/vulkan_calls.c");
  const clientSock = read("runtime/sources/vortek-client/include/socket_utils.h");

  const serverCreate = server.slice(server.indexOf("void vt_handle_vkCreateFence"));
  // Must not force SYNC_FD export on every guest fence (Mali create-signaled footgun).
  assert.doesNotMatch(
    serverCreate.slice(0, serverCreate.indexOf("void vt_handle_vkDestroyFence")),
    /VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT/,
  );

  const serverWait = server.slice(server.indexOf("void vt_handle_vkWaitForFences"));
  const serverWaitBody = serverWait.slice(
    0,
    serverWait.indexOf("void vt_handle_vkCreateSemaphore"),
  );
  // Always host-wait; never vkGetFenceFdKHR for WaitForFences.
  assert.match(serverWaitBody, /vkWaitForFences/);
  assert.doesNotMatch(serverWaitBody, /vkGetFenceFd/);
  assert.match(
    serverWaitBody,
    /send_fds\(context->clientFd, NULL, 0, &result, sizeof\(VkResult\)\)/,
  );

  const clientWait = client.slice(client.indexOf("vt_call_vkWaitForFences"));
  // SUCCESS + 0 FDs is valid (host waited / already signaled), not DEVICE_LOST.
  assert.match(clientWait, /if \(numFds == 0\) return VK_SUCCESS/);
  assert.doesNotMatch(
    clientWait,
    /if \(numFds == 0 \|\| result != VK_SUCCESS\) return VK_ERROR_DEVICE_LOST/,
  );

  // Client send_fds must allow 0-FD data-only replies.
  assert.match(clientSock, /msg_control = NULL/);
  assert.match(clientSock, /numFds > 0 && fds/);
});
