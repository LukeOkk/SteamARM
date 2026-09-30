// Decodes the first track of a media file in Android with MediaExtractor and
// MediaCodec (tests/android/run.sh, the session): the decoder chosen, then
// "decoded N bytes". Built against the stubs next to it; the image's own
// framework runs it (app_process64).
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import java.nio.ByteBuffer;

public class Decode {
    public static void main(String[] a) {
        try {
            run(a);
        } catch (Throwable e) {
            e.printStackTrace(System.out);
        }
    }

    static void run(String[] a) throws Exception {
        long t0 = System.nanoTime();
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(a[0]);
        MediaFormat f = ex.getTrackFormat(0);
        String mime = f.getString("mime");
        ex.selectTrack(0);
        MediaCodec c = MediaCodec.createDecoderByType(mime);
        System.out.println("codec " + c.getName() + " for " + mime);
        c.configure(f, null, null, 0);
        c.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inDone = false;
        long out = 0;
        for (int k = 0; k < 5000; k++) {
            if (!inDone) {
                int i = c.dequeueInputBuffer(10000);
                if (i >= 0) {
                    ByteBuffer b = c.getInputBuffer(i);
                    int n = ex.readSampleData(b, 0);
                    if (n < 0) {
                        c.queueInputBuffer(i, 0, 0, 0, 4);      // BUFFER_FLAG_END_OF_STREAM
                        inDone = true;
                    } else {
                        c.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = c.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                out += info.size;
                c.releaseOutputBuffer(o, false);
                if ((info.flags & 4) != 0) break;
            }
        }
        System.out.println("decoded " + out + " bytes in " + (System.nanoTime() - t0) / 1000000 + " ms");
        c.stop();
        c.release();
        ex.release();
    }
}
