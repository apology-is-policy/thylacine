// SunSpider 1.0.2 under the jsc shell:
//     jsc /webkit/sunspider/run.js -- <marker> [reps]
// Each test checks its own result and throws on a wrong one; jsc's run() gives it a
// fresh realm and returns its time. Prints one line per test (the mean of reps runs),
// then "<marker>-ok-<tests>-<total ms>ms", or "<marker>-fail-<count>" after naming
// each failure.
var marker = arguments[0] || "sunspider";
var reps = arguments.length > 1 ? parseInt(arguments[1], 10) : 3;
var dir = "/webkit/sunspider/";
var names = read(dir + "LIST").split("\n").filter(function (n) { return n.length > 0; });
var total = 0;
var failed = 0;
for (var i = 0; i < names.length; i++) {
    var ms = 0;
    try {
        for (var r = 0; r < reps; r++)
            ms += run(dir + names[i] + ".js");
    } catch (e) {
        failed++;
        print("sunspider " + names[i] + " failed: " + e);
        continue;
    }
    total += ms / reps;
    print("sunspider " + names[i] + " " + (ms / reps).toFixed(1) + " ms");
}
print(failed ? marker + "-fail-" + failed : marker + "-ok-" + names.length + "-" + Math.round(total) + "ms");
