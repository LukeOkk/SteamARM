// Compile-time stand-in for the framework class (tests/android/java/media/Decode.java).
package android.media;
public final class MediaCodec {
    public static final class BufferInfo { public int offset, size, flags; public long presentationTimeUs; }
    public static MediaCodec createDecoderByType(String t) throws java.io.IOException { throw new RuntimeException("stub"); }
    public String getName() { throw new RuntimeException("stub"); }
    public void configure(MediaFormat f, android.view.Surface s, MediaCrypto c, int flags) { throw new RuntimeException("stub"); }
    public void start() { throw new RuntimeException("stub"); }
    public int dequeueInputBuffer(long t) { throw new RuntimeException("stub"); }
    public java.nio.ByteBuffer getInputBuffer(int i) { throw new RuntimeException("stub"); }
    public void queueInputBuffer(int i, int o, int s, long t, int f) { throw new RuntimeException("stub"); }
    public int dequeueOutputBuffer(BufferInfo b, long t) { throw new RuntimeException("stub"); }
    public void releaseOutputBuffer(int i, boolean r) { throw new RuntimeException("stub"); }
    public void stop() { throw new RuntimeException("stub"); }
    public void release() { throw new RuntimeException("stub"); }
}
