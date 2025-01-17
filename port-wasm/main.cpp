// Using SDL2
#include <cheatSystem.h>
#include <emscripten.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "gba.h"
#include "globals.h"
#include "memory.h"
#include "sound.h"
#include "ui.h"
#include <iostream>

#include "../src/utils/colorspacehandler/colorspacehandler.h"
#include "MMU.h"
#include "NDSSystem.h"
#include "SPU.h"
#include "mc.h"
#include "rasterize.h"

uint8_t emuGBuf[0x1000];

void load_image_preferences(void);

int showAudioDebug = 0;

int isQuitting = 0;
int frameDrawn = 0;
int audioSent = 0;
uint32_t frameCount = 0;
int emuFPS = 0;
char savFilePath[512];
char osdText[64];
int osdShowCount = 0;
uint32_t FB[240 * 160];

uint8_t lastSaveBuf[LIBRETRO_SAVE_BUF_LEN];

void systemMessage(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  printf("GBA: %s\n", buf);
}

void systemDrawScreen(void) {
  frameDrawn = 1;
  uint8_t *ptr = (uint8_t *)FB;
  uint16_t *srcPtr = (uint16_t *)pix;
  unsigned padding = 256 - 240;

  for (unsigned y = 0; y < 160; y++) {
    for (unsigned x = 0; x < 240; x++) {
      uint16_t c = *srcPtr;
      uint8_t r = ((c & 0xF800) >> 11) << 3;
      uint8_t g = ((c & 0x7E0) >> 5) << 2;
      uint8_t b = ((c & 0x1F)) << 3;
      ptr[0] = r;
      ptr[1] = g;
      ptr[2] = b;
      ptr[3] = 0xFF;
      ptr += 4;
      srcPtr++;
    }
    srcPtr += padding;
  }
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {
  EM_ASM_({ writeAudio($0, $1); }, finalWave, length / 2);
}

extern "C" {

void emuRunAudio() {
  audioSent = 0;
  while (!audioSent) {
    CPULoop();
  }
}

void emuRunFrame(int key) {
  joy = key;
  UpdateJoypad();
  frameDrawn = 0;
  while (!frameDrawn) {
    CPULoop();
  }
}

int emuUpdateSavChangeFlag() {
  int changed =
      (memcmp(lastSaveBuf, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN) != 0);
  if (changed) {
    memcpy(lastSaveBuf, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);
  }
  return changed;
}
void emuSetSampleRate(int sampleRate) { soundSetSampleRate(sampleRate); }

int emuLoadROM(int romSize) {
  cpuSaveType = 0;
  flashSize = 0x10000;
  enableRtc = false;
  mirroringEnable = false;
  CPUSetupBuffers();
  CPUInit(NULL, false);
  // gba_init();

  load_image_preferences();
  CPUReset();
  soundSetSampleRate(47782);
  soundReset();
  rtcEnable(true);
  flashSetSize(flashSize);
  memcpy(lastSaveBuf, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);
#ifdef USE_FRAME_SKIP
#warning "Frame skip enabled"
  SetFrameskip(0x1);
#endif
  return 0;
}

void emuResetCpu() { CPUReset(); }

void *emuGetSymbol(int id) {
  if (id == 1) {
    return rom;
  }
  if (id == 2) {
    return libretro_save_buf;
  }
  if (id == 3) {
    return FB;
  }
  if (id == 4) {
    return emuGBuf;
  }
  return 0;
}

int emuAddCheat(const char *codeLine) {
  int len = strlen(codeLine);
  if (len == 13) {
    cheatsAddCBACode(codeLine, codeLine);
  } else if (len == 16) {
    cheatsAddGSACode(codeLine, codeLine, true);
  } else {
    return -1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  memset(rom, 0, 32 * 1024 * 1024);
  EM_ASM(wasmReady(););
  return 0;
}
}
CHEATSEXPORT *cheatsExport = NULL;

int emuLastError = 0;

EM_JS(int, aa, (const char *str), { return eval(UTF8ToString(str)); });

extern "C" int bb(int a0, int a1, int a2, int a3) { return 0; }

int DESMUME_SAMPLE_RATE = 48000;
extern double samples_per_hline;

EMUFILE_MEMORY *savFile = new EMUFILE_MEMORY();
u8 *romBuffer;
int romBufferCap = 0;
int romLen;
volatile bool execute = true;
volatile bool paused = false;
// static s16 samplesBuffer[16384 * 2];
static s16 audioBuffer[16384 * 2];
int samplesRead = 0;
int samplesDesired = 0;

void SNDWasmUpdateAudio(s16 *buffer, u32 num_samples) {
  // printf("%d\n", num_samples);
  samplesRead = num_samples;
  memcpy(audioBuffer, buffer, sizeof(s16) * num_samples * 2);
}
u32 SNDWasmGetAudioSpace() { return samplesDesired; }
int SNDWasmInit(int buffersize) { return 0; }
void SNDWasmDeInit() {}
void SNDWasmMuteAudio() {}
void SNDWasmUnMuteAudio() {}
void SNDWasmSetVolume(int volume) {}
void SNDWasmClearBuffer() {}
void SNDWasmFetchSamples(s16 *sampleBuffer, size_t sampleCount,
                         ESynchMode synchMode,
                         ISynchronizingAudioBuffer *theSynchronizer) {
  if (synchMode == ESynchMode_Synchronous) {
    theSynchronizer->enqueue_samples(sampleBuffer, sampleCount);
  }
}

// The NDS reads audio samples in the following format:
//    Format: Unsigned Linear PCM
//    Channels: 1 (mono)
//    Sample Rate: 16000 Hz
//    Sample Resolution: 7-bit
#define MIC_FIFO_SIZE (2048)
#include "fakemic.h"
int fakeMicPos = 0;
int fakeMicEnabled = 0;
uint8_t micFifo[MIC_FIFO_SIZE];
int micFifoPos = 0;
int micFifoLen = 0;
u8 Mic_ReadSample() {
  uint8_t ret = 64;
  if (fakeMicEnabled) {
    ret = fakeMic[fakeMicPos] >> 1;
    fakeMicPos++;
    if (fakeMicPos >= sizeof(fakeMic))
      fakeMicPos = 0;
    return ret;
  }
  if (micFifoLen <= 0) {
    return 64;
  }
  ret = micFifo[micFifoPos];
  micFifoPos = (micFifoPos + 1) % MIC_FIFO_SIZE;
  micFifoLen--;
  // printf("%d\n", ret);
  return ret;
}

SoundInterface_struct SndWasm = {1,
                                 "Wasm Sound Interface",
                                 SNDWasmInit,
                                 SNDWasmDeInit,
                                 SNDWasmUpdateAudio,
                                 SNDWasmGetAudioSpace,
                                 SNDWasmMuteAudio,
                                 SNDWasmUnMuteAudio,
                                 SNDWasmSetVolume,
                                 SNDWasmClearBuffer,
                                 NULL,
                                 NULL};

SoundInterface_struct *SNDCoreList[] = {&SNDDummy, &SndWasm, NULL};
GPU3DInterface *core3DList[] = {&gpu3DNull, &gpu3DRasterize, NULL};

extern "C" {

u32 dstFrameBuffer[2][256 * 192];

static void gpu_screen_to_rgb(u32 *dst) {
  ColorspaceConvertBuffer555To8888Opaque<false, false>(
      (const uint16_t *)GPU->GetDisplayInfo().masterNativeBuffer, dst,
      GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2);
}

unsigned cpu_features_get_core_amount(void) { return 1; }

void setSampleRate(int r) {
  DESMUME_SAMPLE_RATE = r;
  samples_per_hline = (DESMUME_SAMPLE_RATE / 59.8261f) / 263.0f;
}

int main() {
  srand(time(NULL));

  NDS_Init();
  SPU_ChangeSoundCore(1, 16384);

  GPU->Change3DRendererByID(RENDERID_SOFTRASTERIZER);
  cheatsExport = new CHEATSEXPORT();
  printf("wasm ready.\n");
  EM_ASM(wasmReady(););
}

void *prepareRomBuffer(int rl) {
  romLen = rl;
  if (romLen > romBufferCap) {
    romBuffer = (u8 *)realloc((void *)romBuffer, romLen);
    romBufferCap = romLen;
  }
  return romBuffer;
}

void *getSymbol(int id) {
  if (id == 0) {
    // return NDS::ARM7BIOS;
  }
  if (id == 1) {
    // return NDS::ARM9BIOS;
  }
  if (id == 2) {
    // return SPI_Firmware::Firmware;
  }
  if (id == 3) {
    // return NDSCart::CartROM;
  }
  if (id == 4) {
    return dstFrameBuffer;
  }

  if (id == 6) {
    return audioBuffer;
  }
  if (id == 7) {
    return MMU.MAIN_MEM;
  }
  return 0;
}

void reset() { NDS_Reset(); }

int loadROM(int romLen) {

  emuLastError = -2;
  if (!NDS_LoadROM("rom.nds")) {
    return emuLastError;
  }
  SPU_SetSynchMode(ESynchMode_Synchronous, ESynchMethod_N);
  SPU_SetVolume(100);
  cheatsExport->load("usrcheat.dat");
  return 0;
}

static inline void convertFB(u8 *dst, u8 *src) {
  for (int i = 0; i < 256 * 192; i++) {
    uint8_t r = src[2];
    uint8_t g = src[1];
    uint8_t b = src[0];
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = 0xFF;
    dst += 4;
    src += 4;
  }
}

int savGetSize() { return savFile->size(); }

void *savGetPointer(int desiredSize) {
  if (desiredSize > 0) {
    savFile->truncate(desiredSize);
    savFile->fseek(0, SEEK_SET);
  }
  return savFile->buf();
}

int savUpdateChangeFlag() {
  int ret = (savFile->changed) ? 1 : 0;
  savFile->changed = 0;
  return ret;
}

int runFrame(int shouldDraw, u32 keys, int touched, u32 touchX, u32 touchY) {

  if (!shouldDraw) {
    NDS_SkipNextFrame();
  }
  if (touched) {
    NDS_setTouchPos(touchX, touchY);
  } else {
    NDS_releaseTouch();
  }
  NDS_setPad(keys & (1 << 0), keys & (1 << 1), keys & (1 << 2), keys & (1 << 3),
             keys & (1 << 4), keys & (1 << 5), keys & (1 << 6), keys & (1 << 7),
             keys & (1 << 8), keys & (1 << 9), keys & (1 << 10),
             keys & (1 << 11), keys & (1 << 12), keys & (1 << 13));
  fakeMicEnabled = (keys & (1 << 14)) ? 1 : 0;
  int ret = 0;
  NDS_beginProcessingInput();
  NDS_endProcessingInput();
  NDS_exec<false>();

  if (shouldDraw) {
    gpu_screen_to_rgb((u32 *)dstFrameBuffer);
  }

  return ret;
}

void AudioOut_Resample(s16 *inbuf, int inlen, s16 *outbuf, int outlen) {
  float res_incr = inlen / (float)outlen;
  float res_timer = 0;
  int res_pos = 0;
  int prev0 = inbuf[0], prev1 = inbuf[1];

  for (int i = 0; i < outlen; i++) {
    int curr0 = inbuf[res_pos * 2];
    int curr1 = inbuf[res_pos * 2 + 1];
    outbuf[i * 2] = ((curr0));
    outbuf[i * 2 + 1] = ((curr1));

    res_timer += res_incr;
    while (res_timer >= 1.0) {
      prev0 = curr0;
      prev1 = curr1;
      res_timer -= 1.0;
      res_pos++;
    }
  }
}

int fillAudioBuffer(int bufLenToFill) {
  samplesDesired = bufLenToFill;
  samplesRead = 0;
  SPU_Emulate_user();
  /*
  //printf("%d %d\n", samplesRead, bufLenToFill);
  if (samplesRead == bufLenToFill) {
    memcpy(audioBuffer, samplesBuffer, sizeof(s16) * 2 * bufLenToFill);
  } else {
    //printf("should not happen...\n");
    AudioOut_Resample(samplesBuffer, samplesRead, audioBuffer, bufLenToFill);
  }
  */
  return samplesRead;
}

int zlibCompress(u8 *srcBuffer, size_t srcLen, u8 *dstBuffer, size_t dstLen,
                 int level) {
  int ret = compress2(dstBuffer, &dstLen, srcBuffer, srcLen, level);
  if (ret == Z_OK) {
    return dstLen;
  }
  return -1;
}

int zlibDecompress(u8 *srcBuffer, size_t srcLen, u8 *dstBuffer, size_t dstLen) {
  int ret = uncompress(dstBuffer, &dstLen, srcBuffer, srcLen);
  if (ret == Z_OK) {
    return dstLen;
  }
  return -1;
}

std::string cheatListStr = "";
const char *chtGetList() {
  cheatListStr = "";
  CHEATS_LIST *lst = cheatsExport->getCheats();
  int itemCount = cheatsExport->getCheatsNum();
  printf("Cheat list size: %d\n", itemCount);
  for (int i = 0; i < itemCount; i++) {
    // strcat(buf, lst[i].description);
    // strcat(buf, "-c!@");
    cheatListStr += lst[i].description;
    cheatListStr += "-c!@";
  }
  return cheatListStr.c_str();
}

int chtAddItem(int id) {
  if (id < 0) {
    return -1;
  }
  if (id >= cheatsExport->getCheatsNum()) {
    return -1;
  }
  CHEATS_LIST *lst = cheatsExport->getCheats();
  lst[id].enabled = 1;
  cheats->add_AR_Direct(lst[id]);
  return 0;
}

int utilStrLen(const char *p) { return strlen(p); }

int micWriteSamples(uint8_t *buffer, int len) {
  for (int i = 0; i < len; i++) {
    micFifo[(micFifoPos + micFifoLen) % MIC_FIFO_SIZE] = buffer[i];
    micFifoLen++;
    if (micFifoLen > MIC_FIFO_SIZE) {
      // Discard oldest samples
      micFifoLen = MIC_FIFO_SIZE;
      micFifoPos = (micFifoPos + 1) % MIC_FIFO_SIZE;
    }
  }
  return len;
}

int emuSetOpt(int k, int v) {
  if (k == 0) {
    CommonSettings.fwConfig.language = v;
  }
  return 0;
}
}
