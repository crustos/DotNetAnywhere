using System;

class Node { public Node Next; public int Id; public int[] Payload; }

class GcStress {
    static int Main() {
        Node keep = null;
        int checksum = 0;
        for (int round = 0; round < 40; round++) {
            Node head = null;
            for (int i = 0; i < 500; i++) {
                Node n = new Node();
                n.Id = round * 1000 + i;
                n.Payload = new int[(i % 13) + 1];
                n.Payload[0] = n.Id;
                n.Next = head;
                head = n;
                string s = "s" + i;           // short-lived garbage
                checksum += s.Length;
            }
            // keep every 4th round's list alive, drop the rest
            if (round % 4 == 0) {
                Node tail = head;
                while (tail.Next != null) tail = tail.Next;
                tail.Next = keep;
                keep = head;
            }
            GC.Collect();
        }
        int count = 0;
        for (Node n = keep; n != null; n = n.Next) {
            if (n.Payload[0] != n.Id) { Console.WriteLine("CORRUPT"); return 1; }
            checksum += n.Id;
            count++;
        }
        Console.WriteLine("live nodes: " + count);
        Console.WriteLine("checksum: " + checksum);
        return 0;
    }
}
