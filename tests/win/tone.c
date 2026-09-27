// Sound out of a Windows program: 1.5 s of a 440 Hz tone through waveOut
// (Wine -> winepulse -> the Mac's PulseAudio, scripts/audio.sh). The run
// script checks on the Mac side that a stream reached PulseAudio.
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    HWAVEOUT h;
    MMRESULT r = waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    printf("waveOutOpen -> %u\n", r);
    if (r != MMSYSERR_NOERROR) { printf("== tone probe: FAIL\n"); return 1; }
    static short buf[48000 * 3 / 2 * 2];
    for (int i = 0; i < 48000 * 3 / 2; i++)
        buf[2 * i] = buf[2 * i + 1] = (short)(8000 * sin(2 * 3.14159265 * 440 * i / 48000.0));
    WAVEHDR hdr = {(LPSTR)buf, sizeof buf};
    waveOutPrepareHeader(h, &hdr, sizeof hdr);
    r = waveOutWrite(h, &hdr, sizeof hdr);
    printf("waveOutWrite -> %u\n", r);
    Sleep(1700);
    waveOutReset(h);
    waveOutUnprepareHeader(h, &hdr, sizeof hdr);
    waveOutClose(h);
    printf(r == MMSYSERR_NOERROR ? "== tone probe: ok\n" : "== tone probe: FAIL\n");
    return r != MMSYSERR_NOERROR;
}
