#if !LOCALTEST

namespace System.Threading {
	public delegate void WaitCallback(object state);

	// ponytail: no pool; each work item gets its own thread.  Upgrade path: a few worker threads taking items from a queue.
	public static class ThreadPool {
		public static bool QueueUserWorkItem(WaitCallback callBack) { return QueueUserWorkItem(callBack, null); }

		public static bool QueueUserWorkItem(WaitCallback callBack, object state) {
			if (callBack == null) throw new ArgumentNullException("callBack");
			new Thread(s => callBack(s)).Start(state);
			return true;
		}
	}
}

#endif
