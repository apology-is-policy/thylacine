/* Thylacine DX-1 port glue: stub the opusfile (op_*) + speexdsp
 * (speex_resampler_*) entry points that cdrom_image's opus CD-DA decoder
 * (src/libs/decoders/opus.c, unconditionally unity-included and registered in
 * SDL_sound.c's decoder table) references.
 *
 * DX-1 is sound-stubbed (v1.0 non-goal), so decoding .opus CD-audio tracks is
 * out of scope; compiling the full bundled libopusint (ogg+opusfile+opus+
 * speexdsp = 169 C files) would be pure dead weight. These stubs let the port
 * LINK; op_open_callbacks returns NULL so opus_open() fails gracefully and the
 * decoder simply reports "cannot open". Building real libopusint is a DX-3
 * (audio) concern.
 *
 * C linkage: these are C symbols (unmangled), matched to the caller by name.
 * Return types match the real APIs (the ABI-relevant part); parameters are
 * ignored. */
#include <stddef.h>

void *op_open_callbacks(void *source, const void *cb,
                        const unsigned char *initial_data,
                        size_t initial_bytes, int *error) {
    (void)source; (void)cb; (void)initial_data; (void)initial_bytes;
    if (error) *error = -1;            /* OP_EFAULT-ish; opus_open bails */
    return NULL;
}
void op_free(void *of) { (void)of; }
const void *op_head(const void *of, int li) { (void)of; (void)li; return NULL; }
int op_read(void *of, short *pcm, int buf_size, int *li) {
    (void)of; (void)pcm; (void)buf_size; (void)li; return -1;
}
long long op_pcm_total(const void *of, int li) { (void)of; (void)li; return -1; }
int op_pcm_seek(void *of, long long offset) { (void)of; (void)offset; return -1; }
int op_seekable(const void *of) { (void)of; return 0; }

void *speex_resampler_init(unsigned int channels, unsigned int in_rate,
                           unsigned int out_rate, int quality, int *err) {
    (void)channels; (void)in_rate; (void)out_rate; (void)quality;
    if (err) *err = 0; return NULL;
}
void speex_resampler_destroy(void *st) { (void)st; }
int speex_resampler_process_int(void *st, unsigned int channel_index,
                                const short *in, unsigned int *in_len,
                                short *out, unsigned int *out_len) {
    (void)st; (void)channel_index; (void)in; (void)out;
    if (in_len) *in_len = 0;
    if (out_len) *out_len = 0;
    return 0;
}
