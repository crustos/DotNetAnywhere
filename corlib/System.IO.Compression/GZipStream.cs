#if !LOCALTEST

namespace System.IO.Compression {
	public enum CompressionMode {
		Decompress = 0,
		Compress = 1,
	}

	// ponytail: no deflate in DNA's corlib, so a GZipStream cannot be made (programs that only name it still compile and
	// run). Upgrade path: an inflater/deflater here (RFC 1951, a few hundred lines).
	public class GZipStream : Stream {
		public GZipStream(Stream stream, CompressionMode mode) : this(stream, mode, false) { }

		public GZipStream(Stream stream, CompressionMode mode, bool leaveOpen) {
			throw new NotSupportedException("GZipStream: DotNetAnywhere's class library has no deflate.");
		}

		public override bool CanRead { get { return false; } }
		public override bool CanSeek { get { return false; } }
		public override bool CanWrite { get { return false; } }
		public override long Length { get { throw new NotSupportedException(); } }
		public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
		public override void Flush() { }
		public override int Read(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
		public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
		public override void SetLength(long value) { throw new NotSupportedException(); }
		public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
	}
}

#endif
