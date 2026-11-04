using System.Threading.Channels;

namespace Helix.Engine.Transport;

internal sealed class SendQueue(int capacity)
{
	// Wait + TryWrite는 가득 찬 큐를 false로 보고한다. Drop 모드는 사용하지 않는다.
	private readonly Channel<byte[]> _channel = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(capacity)
	{
		SingleReader = true, SingleWriter = false, FullMode = BoundedChannelFullMode.Wait,
		AllowSynchronousContinuations = false
	});
	public bool TryEnqueue(byte[] frame) => _channel.Writer.TryWrite(frame);
	public void Complete() => _channel.Writer.TryComplete();
	public async Task WriteAsync(Stream stream, CancellationToken cancellation)
	{
		await foreach (var frame in _channel.Reader.ReadAllAsync(cancellation).ConfigureAwait(false))
			await stream.WriteAsync(frame, cancellation).ConfigureAwait(false);
	}
}
