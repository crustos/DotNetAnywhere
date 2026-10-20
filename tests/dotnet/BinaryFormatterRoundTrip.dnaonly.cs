using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Formatters.Binary;

// DNA only: .NET 9 and later removed BinaryFormatter, so there is nothing to compare with. A save file, as a Unity
// game writes one (a struct of dictionaries of primitives, structs and arrays), goes to a file and back.
// Self-checking: returns 0 on success, otherwise the number of the failing check.
namespace Game {
    [Serializable] public struct V2 { public float x, y; }
    [Serializable] public class Animal { public string name; }
    [Serializable] public class Dog : Animal { public int barks; [NonSerialized] public int cache = 4; }

    public class Saves {
        [Serializable]
        public struct SaveData {
            public Dictionary<string, bool> bools;
            public Dictionary<string, V2[]> paths;
            public List<int> scores;
            public Animal pet;
            public Animal none;
            public DayOfWeek day;
        }

        public static int Main() {
            SaveData s = new SaveData();
            s.bools = new Dictionary<string, bool>(); s.bools["music"] = true; s.bools["sfx"] = false;
            s.paths = new Dictionary<string, V2[]>();
            s.paths["level 1"] = new V2[] { new V2 { x = 1, y = 2 }, new V2 { x = -3.5f, y = 0 } };
            s.paths["empty"] = new V2[0];
            s.scores = new List<int> { 10, 20 };
            s.pet = new Dog { name = "Rex", barks = 3, cache = 9 };
            s.day = DayOfWeek.Friday;

            string path = "bf_roundtrip.bin";
            using (FileStream f = new FileStream(path, FileMode.Create)) new BinaryFormatter().Serialize(f, s);
            SaveData r;
            using (FileStream f = new FileStream(path, FileMode.Open)) r = (SaveData)new BinaryFormatter().Deserialize(f);
            File.Delete(path);

            if (r.bools.Count != 2 || !r.bools["music"] || r.bools["sfx"]) return 1;
            if (r.paths["level 1"].Length != 2 || r.paths["level 1"][1].x != -3.5f || r.paths["level 1"][0].y != 2) return 2;
            if (r.paths["empty"].Length != 0) return 3;
            if (r.scores.Count != 2 || r.scores[1] != 20) return 4;
            Dog dog = r.pet as Dog;
            if (dog == null || dog.name != "Rex" || dog.barks != 3) return 5;
            if (dog.cache != 0) return 6;                                  // [NonSerialized] is not written, and no constructor runs
            if (r.none != null) return 7;
            if (r.day != DayOfWeek.Friday) return 8;

            try { new BinaryFormatter().Deserialize(new FileStream("bf_roundtrip.txt", FileMode.Create)); return 9; }
            catch (SerializationException) { }
            File.Delete("bf_roundtrip.txt");
            return 0;
        }
    }
}
