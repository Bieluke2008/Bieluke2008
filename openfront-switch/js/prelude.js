// Host polyfills for QuickJS. Runs before the engine bundle, which builds a
// TextEncoder/TextDecoder at module load. `host` is provided by the native
// side (src/js_host.c); under Node, scripts/node-host.mjs provides it.
(function () {
  var g = globalThis;
  var h = g.host;
  if (typeof g.console === "undefined" || !g.console.log) {
    var fmt = function (args) {
      var out = [];
      for (var i = 0; i < args.length; i++) {
        var a = args[i];
        if (a instanceof Error) out.push(a.message + "\n" + (a.stack || ""));
        else if (typeof a === "object") {
          try {
            out.push(JSON.stringify(a));
          } catch (e) {
            out.push(String(a));
          }
        } else out.push(String(a));
      }
      return out.join(" ");
    };
    var mk = function (level) {
      return function () {
        h.log(level + fmt(arguments));
      };
    };
    g.console = {
      log: mk(""),
      info: mk(""),
      debug: function () {},
      warn: mk("[warn] "),
      error: mk("[error] "),
      trace: function () {},
      time: function () {},
      timeEnd: function () {},
    };
  }
  if (typeof g.performance === "undefined") {
    g.performance = { now: h.now };
  }
  if (typeof g.TextEncoder === "undefined") {
    g.TextEncoder = function TextEncoder() {};
    g.TextEncoder.prototype.encode = function (str) {
      str = str === undefined ? "" : String(str);
      var out = [];
      for (var i = 0; i < str.length; i++) {
        var c = str.charCodeAt(i);
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < str.length) {
          var d = str.charCodeAt(i + 1);
          if (d >= 0xdc00 && d < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
            i++;
          }
        }
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c < 0x10000)
          out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        else
          out.push(
            0xf0 | (c >> 18),
            0x80 | ((c >> 12) & 63),
            0x80 | ((c >> 6) & 63),
            0x80 | (c & 63),
          );
      }
      return new Uint8Array(out);
    };
    g.TextEncoder.prototype.encodeInto = function (str, dest) {
      var bytes = this.encode(str);
      var n = Math.min(bytes.length, dest.length);
      dest.set(bytes.subarray(0, n));
      return { read: str.length, written: n };
    };
  }
  if (typeof g.TextDecoder === "undefined") {
    g.TextDecoder = function TextDecoder() {};
    g.TextDecoder.prototype.decode = function (buf) {
      if (!buf) return "";
      var b = buf instanceof Uint8Array ? buf : new Uint8Array(buf.buffer || buf);
      var s = "";
      for (var i = 0; i < b.length; ) {
        var c = b[i++];
        if (c >= 0xf0) {
          c =
            ((c & 7) << 18) |
            ((b[i++] & 63) << 12) |
            ((b[i++] & 63) << 6) |
            (b[i++] & 63);
        } else if (c >= 0xe0) {
          c = ((c & 15) << 12) | ((b[i++] & 63) << 6) | (b[i++] & 63);
        } else if (c >= 0xc0) {
          c = ((c & 31) << 6) | (b[i++] & 63);
        }
        s += String.fromCodePoint(c);
      }
      return s;
    };
  }
})();
