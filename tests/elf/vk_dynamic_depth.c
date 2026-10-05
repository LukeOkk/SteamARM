// Dynamic depth/stencil correctness and encoding benchmark. Alternating
// ALWAYS/NEVER depth comparison must keep the same pixels, with cached Metal
// states or without them. A second pass exercises both stencil faces, masks,
// operations, reference values, depth writes and more than 64 distinct states.
// Usage: vk_dynamic_depth [even draw repetitions, default 20000].
// Report recording + submission + completion; neither metric is game FPS.
#include <stdint.h>
#include "vk_passes_spv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    VkResult r_ = (x);                                                         \
    if (r_ != VK_SUCCESS) {                                                    \
      printf("%s -> %d\n== vk_dynamic_depth: FAIL\n", #x, r_);                 \
      return 1;                                                                \
    }                                                                          \
  } while (0)
enum { W = 64, H = 64 };

static int repeats = 20000;
static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static VkDevice dev;
static VkPhysicalDevice pd;
static VkDeviceMemory image_memory[3];
static unsigned image_memory_count;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
      return i;
  return 0;
}

static int make_image(VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img,
                      VkImageView *view) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           0,
                           0,
                           VK_IMAGE_TYPE_2D,
                           fmt,
                           {W, H, 1},
                           1,
                           1,
                           VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_TILING_OPTIMAL,
                           usage};
  CHECK(vkCreateImage(dev, &ici, 0, img));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, *img, &mr);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0,
                              mr.size, mem_type(mr.memoryTypeBits, 0)};
  VkDeviceMemory mem;
  CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
  CHECK(vkBindImageMemory(dev, *img, mem, 0));
  image_memory[image_memory_count++] = mem;
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               0,
                               0,
                               *img,
                               VK_IMAGE_VIEW_TYPE_2D,
                               fmt,
                               {0},
                               {aspect, 0, 1, 0, 1}};
  CHECK(vkCreateImageView(dev, &vci, 0, view));
  return 0;
}

static void transition(VkCommandBuffer cb, VkImage img,
                       VkImageAspectFlags aspect, VkImageLayout from,
                       VkImageLayout to) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                            0,
                            0,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT,
                            from,
                            to,
                            VK_QUEUE_FAMILY_IGNORED,
                            VK_QUEUE_FAMILY_IGNORED,
                            img,
                            {aspect, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1,
                       &b);
}

// One pass on `color` (and `depth`, if any): begin, draw `rgba` into `draw`
// if it has a width, end.
static void pass(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout pl,
                 VkImageView color, VkImageView depth, VkAttachmentLoadOp load,
                 const float clear[4], VkRect2D area, VkRect2D draw,
                 const float rgba[4]) {
  VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                  0,
                                  color,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  0,
                                  0,
                                  0,
                                  load,
                                  VK_ATTACHMENT_STORE_OP_STORE};
  if (clear)
    memcpy(ca.clearValue.color.float32, clear,
           sizeof ca.clearValue.color.float32);
  // The depth buffer is loaded as it is and thrown away: a store operation
  // of DONT_CARE the driver may now honour.
  VkRenderingAttachmentInfo da = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      0,
      depth,
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      0,
      0,
      0,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      VK_ATTACHMENT_STORE_OP_DONT_CARE};
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO,
                        0,
                        0,
                        area,
                        1,
                        0,
                        1,
                        &ca,
                        depth ? &da : 0,
                        depth ? &da : 0};
  vkCmdBeginRendering(cb, &ri);
  if (draw.extent.width) {
    VkViewport vp = {0, 0, W, H, 0, 1};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &draw);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rgba);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, depth != VK_NULL_HANDLE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    for (int i = 0; i < repeats; ++i) {
      vkCmdSetDepthCompareOp(cb, (i & 1) ? VK_COMPARE_OP_ALWAYS
                                         : VK_COMPARE_OP_NEVER);
      vkCmdDraw(cb, 3, 1, 0, 0);
    }
  }
  vkCmdEndRendering(cb);
}

static int pixel(const unsigned char *p, int x, int y, unsigned r, unsigned g,
                 unsigned b, const char *what) {
  const unsigned char *q = p + 4 * (y * W + x);
  if (q[0] == r && q[1] == g && q[2] == b)
    return 0;
  printf(
      "(%d,%d) is %u,%u,%u, wanted %u,%u,%u: %s\n== vk_dynamic_depth: FAIL\n",
      x, y, q[0], q[1], q[2], r, g, b, what);
  return 1;
}

int main(int argc, char **argv) {
  if (argc > 1)
    repeats = atoi(argv[1]);
  if (repeats < 2 || (repeats & 1))
    return 2;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           0,
                           "vk_passes",
                           1,
                           0,
                           0,
                           VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0,
                              &app};
  VkInstance inst;
  CHECK(vkCreateInstance(&ici, 0, &inst));
  uint32_t n = 1;
  vkEnumeratePhysicalDevices(inst, &n, &pd);
  if (!n) {
    printf("no device\n== vk_dynamic_depth: FAIL\n");
    return 1;
  }
  float prio = 1;
  VkDeviceQueueCreateInfo q = {
      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
  VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
  eds.extendedDynamicState = VK_TRUE;
  VkPhysicalDeviceVulkan13Features f13 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.pNext = &eds;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1,
                            &q};
  const char *exts[] = {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = exts;
  CHECK(vkCreateDevice(pd, &dci, 0, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, 0, 0, &queue);

  VkImage t, u, d;
  VkImageView tv, uv, dv;
  VkImageUsageFlags cu =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (make_image(VK_FORMAT_R8G8B8A8_UNORM, cu, VK_IMAGE_ASPECT_COLOR_BIT, &t,
                 &tv) ||
      make_image(VK_FORMAT_R8G8B8A8_UNORM, cu, VK_IMAGE_ASPECT_COLOR_BIT, &u,
                 &uv) ||
      make_image(VK_FORMAT_D32_SFLOAT_S8_UINT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT), &d,
                 &dv))
    return 1;

  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0,
                            2 * W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer buf;
  CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  VkMemoryAllocateInfo mai = {
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
      mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  VkDeviceMemory bmem;
  CHECK(vkAllocateMemory(dev, &mai, 0, &bmem));
  CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

  VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pli = {
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pr};
  VkPipelineLayout pl;
  CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
  VkShaderModuleCreateInfo vi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0,
                                 0, sizeof vkp_vert, vkp_vert};
  VkShaderModuleCreateInfo fi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0,
                                 0, sizeof vkp_frag, vkp_frag};
  VkShaderModule vs, fs;
  CHECK(vkCreateShaderModule(dev, &vi, 0, &vs));
  CHECK(vkCreateShaderModule(dev, &fi, 0, &fs));
  VkPipelineShaderStageCreateInfo st[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"},
  };
  VkPipelineVertexInputStateCreateInfo vin = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkPipelineViewportStateCreateInfo vps = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
  VkPipelineRasterizationStateCreateInfo rs = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.lineWidth = 1;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  VkPipelineMultisampleStateCreateInfo ms = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0,
      VK_SAMPLE_COUNT_1_BIT};
  VkPipelineDepthStencilStateCreateInfo dss = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dss.depthTestEnable = VK_TRUE;
  dss.depthWriteEnable = VK_TRUE;
  dss.depthCompareOp = VK_COMPARE_OP_LESS;
  dss.stencilTestEnable = VK_TRUE;
  dss.front.compareOp = dss.back.compareOp = VK_COMPARE_OP_ALWAYS;
  dss.front.compareMask = dss.back.compareMask = 0xff;
  dss.front.writeMask = dss.back.writeMask = 0xff;
  VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xf};
  VkPipelineColorBlendStateCreateInfo cbs = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      0,
      0,
      0,
      0,
      1,
      &ba};
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                          VK_DYNAMIC_STATE_SCISSOR,
                          VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                          VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                          VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                          VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                          VK_DYNAMIC_STATE_STENCIL_OP,
                          VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                          VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                          VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                          VK_DYNAMIC_STATE_FRONT_FACE};
  VkPipelineDynamicStateCreateInfo dsi = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0,
      sizeof(dyn) / sizeof(dyn[0]), dyn};
  VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
  // Two pipelines: with the depth buffer (T) and without (U has none).
  VkPipeline pipe[2];
  for (int k = 0; k < 2; k++) {
    VkPipelineRenderingCreateInfo prc = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        0,
        0,
        1,
        &cf,
        k ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT_S8_UINT,
        k ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT_S8_UINT};
    VkGraphicsPipelineCreateInfo gi = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        &prc,
        0,
        2,
        st,
        &vin,
        &ia,
        0,
        &vps,
        &rs,
        &ms,
        &dss,
        &cbs,
        &dsi,
        pl};
    CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe[k]));
  }

  VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
  VkCommandBufferAllocateInfo cai = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool,
      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cb;
  CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  0,
                                  VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  double record_start = now_ms();
  CHECK(vkBeginCommandBuffer(cb, &cbi));
  transition(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  transition(cb, u, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  transition(cb, d, (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
             VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

  const float black[4] = {0, 0, 0, 1}, blue[4] = {0, 0, 1, 1},
              white[4] = {1, 1, 1, 1};
  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1},
              yellow[4] = {1, 1, 0, 1};
  const VkRect2D whole = {{0, 0}, {W, H}}, none = {{0, 0}, {0, 0}};
  pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_CLEAR, black, whole,
       (VkRect2D){{0, 0}, {32, H}}, red); // A
  pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_LOAD, 0, whole,
       (VkRect2D){{32, 0}, {32, H}}, green); // B
  pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_CLEAR, blue,
       (VkRect2D){{0, 0}, {16, 16}}, none, 0); // C
  pass(cb, pipe[1], pl, uv, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, white, whole, none,
       0); // D
  pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_LOAD, 0, whole,
       (VkRect2D){{40, 40}, {16, 16}}, yellow); // E

  transition(cb, t, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  transition(cb, u, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy c0 = {
      0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
  VkBufferImageCopy c1 = c0;
  c1.bufferOffset = W * H * 4;
  vkCmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                         &c0);
  vkCmdCopyImageToBuffer(cb, u, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                         &c1);
  CHECK(vkEndCommandBuffer(cb));
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
  double record_ms = now_ms() - record_start;
  double start = now_ms();
  CHECK(vkQueueSubmit(queue, 1, &si, 0));
  double submitted = now_ms();
  CHECK(vkQueueWaitIdle(queue));
  printf("dynamic depth: %d draws, record %.3f ms, submit %.3f ms, total %.3f "
         "ms\n",
         3 * repeats, record_ms, submitted - start, now_ms() - record_start);

  void *map;
  CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map));
  const unsigned char *pt = map, *pu = pt + W * H * 4;
  if (pixel(pt, 8, 8, 0, 0, 255, "C's clear inside the continued pass") ||
      pixel(pt, 24, 8, 255, 0, 0, "A's draw outside C's area") ||
      pixel(pt, 8, 24, 255, 0, 0, "A's draw below C's area") ||
      pixel(pt, 40, 8, 0, 255, 0, "B's draw, continuing A") ||
      pixel(pt, 44, 44, 255, 255, 0,
            "E's draw, back on T after another target") ||
      pixel(pt, 56, 56, 0, 255, 0, "B's draw outside E's") ||
      pixel(pu, 32, 32, 255, 255, 255, "D's clear of the other target"))
    return 1;
  vkUnmapMemory(dev, bmem);
  // 256 independently cleared cells: cache capacity and keys, both faces.
  CHECK(vkResetCommandBuffer(cb, 0));
  CHECK(vkBeginCommandBuffer(cb, &cbi));
  transition(cb, t, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                  0,
                                  tv,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  0,
                                  0,
                                  0,
                                  VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  VK_ATTACHMENT_STORE_OP_STORE};
  ca.clearValue.color.float32[3] = 1;
  VkRenderingAttachmentInfo da = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      0,
      dv,
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      0,
      0,
      0,
      VK_ATTACHMENT_LOAD_OP_CLEAR,
      VK_ATTACHMENT_STORE_OP_DONT_CARE};
  da.clearValue.depthStencil.depth = 1;
  da.clearValue.depthStencil.stencil = 0x5a;
  VkRenderingInfo ri = {
      VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, whole, 1, 0, 1, &ca, &da, &da};
  vkCmdBeginRendering(cb, &ri);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[0]);
  VkViewport vp = {0, 0, W, H, 0, 1};
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetDepthTestEnable(cb, VK_TRUE);
  vkCmdSetStencilTestEnable(cb, VK_TRUE);
  unsigned char expected[256][3];
  for (unsigned i = 0; i < 256; ++i) {
    VkRect2D cell = {{(i % 16) * 4, (i / 16) * 4}, {4, 4}};
    vkCmdSetScissor(cb, 0, 1, &cell);
    vkCmdSetFrontFace(cb, (i & 1) ? VK_FRONT_FACE_CLOCKWISE
                                  : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthWriteEnable(cb, (i & 2) != 0);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS);
    // The read mask alone supplies 256 distinct keys; the other face
    // differs so a cache that omits front/back state is not sufficient.
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, i);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, 255 - i);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0x5a);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_ZERO,
                      VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
                      VK_COMPARE_OP_EQUAL);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, VK_STENCIL_OP_REPLACE,
                      VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO,
                      VK_COMPARE_OP_EQUAL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(cb, 3, 1, 0, 0); // 0.5 < 1 and stencil == reference: red.
    // This green draw must fail equality in depth exactly when red wrote.
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, green);
    vkCmdDraw(cb, 3, 1, 0, 0);
    expected[i][0] = (i & 2) ? 255 : 0;
    expected[i][1] = (i & 2) ? 0 : 255;
    expected[i][2] = 0;
  }
  vkCmdEndRendering(cb);
  transition(cb, t, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkCmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                         &c0);
  CHECK(vkEndCommandBuffer(cb));
  CHECK(vkQueueSubmit(queue, 1, &si, 0));
  CHECK(vkQueueWaitIdle(queue));
  CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map));
  for (unsigned i = 0; i < 256; i++)
    if (pixel(map, (i % 16) * 4 + 1, (i / 16) * 4 + 1, expected[i][0],
              expected[i][1], expected[i][2], "dynamic depth/stencil cell"))
      return 1;
  vkUnmapMemory(dev, bmem);

  vkDestroyCommandPool(dev, pool, 0);
  for (int k = 0; k < 2; k++)
    vkDestroyPipeline(dev, pipe[k], 0);
  vkDestroyPipelineLayout(dev, pl, 0);
  vkDestroyShaderModule(dev, vs, 0);
  vkDestroyShaderModule(dev, fs, 0);
  vkDestroyBuffer(dev, buf, 0);
  vkFreeMemory(dev, bmem, 0);
  vkDestroyImageView(dev, tv, 0);
  vkDestroyImageView(dev, uv, 0);
  vkDestroyImageView(dev, dv, 0);
  vkDestroyImage(dev, t, 0);
  vkDestroyImage(dev, u, 0);
  vkDestroyImage(dev, d, 0);
  for (unsigned i = 0; i < image_memory_count; i++)
    vkFreeMemory(dev, image_memory[i], 0);
  vkDestroyDevice(dev, 0);
  vkDestroyInstance(inst, 0);
  printf("== vk_dynamic_depth: ok\n");
  return 0;
}
