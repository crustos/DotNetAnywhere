#if !LOCALTEST

namespace System.IO {
	public static class File {

		public static FileStream OpenRead(string path) {
			return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
		}

		public static StreamReader OpenText(string path) {
			return new StreamReader(path);
		}

		public static byte[] ReadAllBytes(string path) {
			int error;
			IntPtr f = FileInternal.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read, out error);
			if (error != 0) throw FileInternal.GetException(error, path);
			byte[] buf = new byte[4096];
			int len = 0;
			while (true) {
				if (len == buf.Length) {
					byte[] bigger = new byte[buf.Length * 2];
					Array.Copy(buf, bigger, len);
					buf = bigger;
				}
				int n = FileInternal.Read(f, buf, len, buf.Length - len, out error);
				if (error != 0 || n <= 0) break;
				len += n;
			}
			int closeError;
			FileInternal.Close(f, out closeError);
			if (error != 0) throw FileInternal.GetException(error, path);
			byte[] all = new byte[len];
			Array.Copy(buf, all, len);
			return all;
		}

		public static void WriteAllBytes(string path, byte[] bytes) { Write(path, bytes, FileMode.Create); }

		static void Write(string path, byte[] bytes, FileMode mode) {
			int error;
			IntPtr f = FileInternal.Open(path, mode, FileAccess.Write, FileShare.None, out error);
			if (error != 0) throw FileInternal.GetException(error, path);
			int done = 0;
			while (done < bytes.Length) {
				int n = FileInternal.Write(f, bytes, done, bytes.Length - done, out error);
				if (error != 0 || n <= 0) break;
				done += n;
			}
			int closeError;
			FileInternal.Close(f, out closeError);
			if (error != 0) throw FileInternal.GetException(error, path);
		}

		/// <summary>The file's text, as UTF-8 (a byte order mark is skipped).</summary>
		public static string ReadAllText(string path) {
			byte[] b = ReadAllBytes(path);
			int i = (b.Length >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
			System.Text.StringBuilder s = new System.Text.StringBuilder(b.Length);
			while (i < b.Length) {
				int c = b[i++];
				if (c >= 0xF0 && i + 2 < b.Length + 0) {
					int cp = ((c & 0x07) << 18) | ((b[i] & 0x3F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F);
					i += 3;
					cp -= 0x10000;
					s.Append((char)(0xD800 + (cp >> 10)));
					s.Append((char)(0xDC00 + (cp & 0x3FF)));
				} else if (c >= 0xE0 && i + 1 < b.Length) {
					s.Append((char)(((c & 0x0F) << 12) | ((b[i] & 0x3F) << 6) | (b[i + 1] & 0x3F)));
					i += 2;
				} else if (c >= 0xC0 && i < b.Length) {
					s.Append((char)(((c & 0x1F) << 6) | (b[i] & 0x3F)));
					i += 1;
				} else {
					s.Append((char)c);
				}
			}
			return s.ToString();
		}

		static byte[] Utf8(string text) {
			byte[] b = new byte[text.Length * 3];
			int n = 0;
			for (int i = 0; i < text.Length; i++) {
				int c = text[i];
				if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.Length) {
					int cp = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
					if (n + 4 > b.Length) { byte[] bigger = new byte[b.Length * 2 + 4]; Array.Copy(b, bigger, n); b = bigger; }
					b[n++] = (byte)(0xF0 | (cp >> 18)); b[n++] = (byte)(0x80 | ((cp >> 12) & 0x3F));
					b[n++] = (byte)(0x80 | ((cp >> 6) & 0x3F)); b[n++] = (byte)(0x80 | (cp & 0x3F));
				} else if (c >= 0x800) {
					b[n++] = (byte)(0xE0 | (c >> 12)); b[n++] = (byte)(0x80 | ((c >> 6) & 0x3F)); b[n++] = (byte)(0x80 | (c & 0x3F));
				} else if (c >= 0x80) {
					b[n++] = (byte)(0xC0 | (c >> 6)); b[n++] = (byte)(0x80 | (c & 0x3F));
				} else {
					b[n++] = (byte)c;
				}
			}
			byte[] all = new byte[n];
			Array.Copy(b, all, n);
			return all;
		}

		/// <summary>Writes the text as UTF-8 (no byte order mark), replacing the file.</summary>
		public static void WriteAllText(string path, string contents) { Write(path, Utf8(contents ?? ""), FileMode.Create); }

		public static void AppendAllText(string path, string contents) { Write(path, Utf8(contents ?? ""), FileMode.Append); }

		/// <summary>No error when the file is not there, as in .NET.</summary>
		public static void Delete(string path) {
			if (path == null) throw new ArgumentNullException("path");
			int error;
			FileInternal.Delete(path, false, out error);
			if (error != 0 && error != FileInternal.ERROR_FILE_NOT_FOUND) throw FileInternal.GetException(error, path);
		}

		public static void Copy(string sourceFileName, string destFileName) { Copy(sourceFileName, destFileName, false); }

		public static void Copy(string sourceFileName, string destFileName, bool overwrite) {
			if (!overwrite && Exists(destFileName)) throw new IOException("The file '" + destFileName + "' already exists.");
			WriteAllBytes(destFileName, ReadAllBytes(sourceFileName));
		}

		public static bool Exists(string path) {
			if (string.IsNullOrEmpty(path)) {
				return false;
			}
			int error;
			return FileInternal.ExistsFile(path, out error);
		}

	}
}

#endif
