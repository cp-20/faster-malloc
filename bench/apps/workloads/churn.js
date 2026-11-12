// churn.js -- allocation-heavy Node.js workload.
//
// V8 manages JS-object memory itself (its own heap and GC), so this
// workload deliberately leans on the parts that reach libc malloc:
// JSON.parse/stringify of a large payload (external strings and the
// parse temporaries), Buffer churn above the 8 KiB pool size (backed
// by malloc), and a large array of small objects for GC pressure.
// Prints a checksum so corruption changes the output.
const fs = require('fs');

const raw = fs.readFileSync(process.argv[2], 'utf8');
let acc = 0;

for (let r = 0; r < 3; r++) {
    const data = JSON.parse(raw);
    acc += data.length;
    const out = JSON.stringify(data);
    acc += out.length % 1000;
}

const bufs = [];
for (let i = 0; i < 4000; i++) {
    const b = Buffer.alloc(16384 + (i % 4) * 16384, i % 251);
    bufs.push(b);
    if (bufs.length > 48) bufs.shift();
}
acc += bufs.reduce((s, b) => s + b.length, 0) % 100000;

const objs = [];
for (let i = 0; i < 500000; i++) {
    objs.push({ id: i, k: 'v' + (i % 5000), xs: [i, i + 1, i + 2] });
    if (objs.length > 8192) objs.splice(0, 4096);
}
acc += objs.length;

console.log('node-churn checksum', acc);
