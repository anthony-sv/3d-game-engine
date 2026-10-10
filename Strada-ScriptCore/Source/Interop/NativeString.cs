using System.Text;

namespace Strada.Interop;

// Strings cross the native boundary as UTF-8 bytes and a length.
internal static unsafe class NativeString
{
	internal static byte[] ToUtf8(string text) => Encoding.UTF8.GetBytes(text);

	internal static string FromUtf8(byte* text, int length) => text != null && length > 0 ? Encoding.UTF8.GetString(text, length) : string.Empty;
}
