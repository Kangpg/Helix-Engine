using System.Net.Sockets;
using Helix.Engine.Protocol;

namespace Helix.Engine.Transport;

internal sealed class TcpSession : ISessionTransport
{
	private readonly TcpClient _client;
	private readonly FrameCodec _codec;
	private readonly SendQueue _send;
	private readonly CancellationTokenSource _stop = new();
	private readonly TaskCompletionSource _ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
	private int _closed;
	private CloseReason _reason = CloseReason.PeerClosed;
	public Session Handle { get; }
	public Task Completion => _ended.Task;

	public TcpSession(ulong id, TcpClient client, EngineConfig config)
	{
		_client = client;
		_client.NoDelay = true;
		_codec = new FrameCodec(config);
		_send = new SendQueue(config.MaxSendQueue);
		Handle = new Session(id, this);
	}

	public bool Send(ReadOnlySpan<byte> message)
	{
		if (Volatile.Read(ref _closed) != 0) return false;
		try { return _send.TryEnqueue(_codec.Encode(message)); }
		catch (InvalidDataException) { return false; }
	}

	public void Close(CloseReason reason)
	{
		if (Interlocked.CompareExchange(ref _closed, 1, 0) != 0) return;
		_reason = reason;
		_send.Complete();
		_stop.Cancel();
		_client.Dispose();
	}

	public async Task RunAsync(IHelixApp app, Action<Exception> onError, Action onEnded)
	{
		Task? writer = null;
		try
		{
			app.OnConnect(Handle);
			var stream = _client.GetStream();
			writer = WriteAsync(stream);
			while (!_stop.IsCancellationRequested)
			{
				var message = await _codec.ReadAsync(stream, _stop.Token).ConfigureAwait(false);
				if (message is null) break;
				app.OnMessage(Handle, message);
			}
		}
		catch (InvalidDataException) { Close(CloseReason.ProtocolError); }
		catch (EndOfStreamException) { Close(CloseReason.ProtocolError); }
		catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
		catch (ObjectDisposedException) when (_stop.IsCancellationRequested) { }
		catch (Exception error) { Close(CloseReason.IoError); onError(error); }
		finally
		{
			Close(_reason);
			if (writer is not null)
			{
				try { await writer.ConfigureAwait(false); }
				catch (Exception error) when (error is OperationCanceledException or IOException or SocketException or ObjectDisposedException) { }
			}
			try { app.OnDisconnect(Handle, _reason); }
			catch (Exception error) { onError(error); }
			finally { onEnded(); _ended.TrySetResult(); }
		}
	}

	private async Task WriteAsync(Stream stream)
	{
		try { await _send.WriteAsync(stream, _stop.Token).ConfigureAwait(false); }
		catch { Close(CloseReason.IoError); throw; }
	}
}
