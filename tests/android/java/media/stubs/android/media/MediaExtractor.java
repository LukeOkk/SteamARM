// Compile-time stand-in for the framework class (tests/android/java/media/Decode.java).
package android.media;
public final class MediaExtractor {
    public MediaExtractor() { throw new RuntimeException("stub"); }
    public void setDataSource(String p) throws java.io.IOException { throw new RuntimeException("stub"); }
    public int getTrackCount() { throw new RuntimeException("stub"); }
    public MediaFormat getTrackFormat(int i) { throw new RuntimeException("stub"); }
    public void selectTrack(int i) { throw new RuntimeException("stub"); }
    public int readSampleData(java.nio.ByteBuffer b, int off) { throw new RuntimeException("stub"); }
    public boolean advance() { throw new RuntimeException("stub"); }
    public long getSampleTime() { throw new RuntimeException("stub"); }
    public void release() { throw new RuntimeException("stub"); }
}
