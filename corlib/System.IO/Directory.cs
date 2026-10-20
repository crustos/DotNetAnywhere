#if !LOCALTEST

namespace System.IO {
	public static class Directory {

		public static bool Exists(string path) {
			if (string.IsNullOrEmpty(path)) return false;
			int error;
			return FileInternal.ExistsDirectory(path, out error);
		}

		/// <summary>Makes the directory and any parents it lacks.</summary>
		public static DirectoryInfo CreateDirectory(string path) {
			if (string.IsNullOrEmpty(path)) throw new ArgumentException("Path is empty");
			int slash = path.IndexOf('/', 1);
			while (true) {
				string part = slash < 0 ? path : path.Substring(0, slash);
				if (part.Length > 0 && !Exists(part)) {
					int error;
					FileInternal.CreateDirectory(part, out error);
					if (error != 0) throw FileInternal.GetException(error, part);
				}
				if (slash < 0) break;
				slash = path.IndexOf('/', slash + 1);
			}
			return new DirectoryInfo(path);
		}

		public static string GetCurrentDirectory() {
			int error;
			string dir = FileInternal.GetCurrentDirectory(out error);
			if (error != 0) {
				throw FileInternal.GetException(error, string.Empty);
			}
			return dir;
		}

		public static string[] GetFiles(string path) {
			return GetFiles(path, "*");
		}

		public static string[] GetFiles(string path, string pattern) {
			return GetFileSystemEntries(path, pattern, FileAttributes.Directory, 0);
		}

		public static string[] GetFiles(string path, string pattern, SearchOption searchOption) {
			if (searchOption == SearchOption.TopDirectoryOnly) return GetFiles(path, pattern);
			System.Collections.Generic.List<string> all = new System.Collections.Generic.List<string>(GetFiles(path, pattern));
			foreach (string dir in GetDirectories(path)) all.AddRange(GetFiles(dir, pattern, searchOption));
			return all.ToArray();
		}

		public static string[] GetDirectories(string path) {
			return GetDirectories(path, "*");
		}

		public static string[] GetDirectories(string path, string pattern) {
			return GetFileSystemEntries(path, pattern, FileAttributes.Directory, FileAttributes.Directory);
		}

		public static void Delete(string path) {
			Delete(path, false);
		}

		public static void Delete(string path, bool recursive) {
			if (!Exists(path)) throw new DirectoryNotFoundException("Could not find a part of the path '" + path + "'.");
			if (recursive) {
				foreach (string f in GetFiles(path)) File.Delete(f);
				foreach (string d in GetDirectories(path)) Delete(d, true);
			}
			int error;
			FileInternal.Delete(path, true, out error);
			if (error != 0) throw new IOException("The directory is not empty or cannot be deleted: '" + path + "'.");
		}

		private static string[] GetFileSystemEntries(string path, string pattern, FileAttributes mask, FileAttributes attrs) {
			if (path == null || pattern == null)
				throw new ArgumentNullException();

			if (pattern == String.Empty)
				return new string[] { };

			if (path.Trim() == "")
				throw new ArgumentException("The Path does not have a valid format");

			string wild = Path.Combine(path, pattern);
			string wildpath = Path.GetDirectoryName(wild);
			if (wildpath.IndexOfAny(Path.InvalidPathChars) != -1)
				throw new ArgumentException("Path contains invalid characters");

			if (wildpath.IndexOfAny(Path.InvalidPathChars) != -1) {
				if (path.IndexOfAny(SearchPattern.InvalidChars) == -1)
					throw new ArgumentException("Path contains invalid characters", "path");

				throw new ArgumentException("Pattern contains invalid characters", "pattern");
			}

			int error;
			if (!FileInternal.ExistsDirectory(wildpath, out error)) {
				if (error == FileInternal.Error_OK) {
					int file_error;
					if (FileInternal.ExistsFile(wildpath, out file_error)) {
						return new string[] { wildpath };
					}
				}

				if (error != FileInternal.ERROR_PATH_NOT_FOUND)
					throw FileInternal.GetException(error, wildpath);

				if (wildpath.IndexOfAny(SearchPattern.WildcardChars) == -1)
					throw new DirectoryNotFoundException("Directory '" + wildpath + "' not found.");

				if (path.IndexOfAny(SearchPattern.WildcardChars) == -1)
					throw new ArgumentException("Pattern is invalid", "pattern");

				throw new ArgumentException("Path is invalid", "path");
			}

			string path_with_pattern = Path.Combine(wildpath, pattern);
			string[] result = FileInternal.GetFileSystemEntries(path, path_with_pattern, attrs, mask, out error);
			if (error != 0)
				throw FileInternal.GetException(error, wildpath);

			return result;
		}

	}
}

#endif
