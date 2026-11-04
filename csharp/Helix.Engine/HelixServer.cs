using Helix.Engine.Transport;

namespace Helix.Engine;

// 호스트는 추상 전송과 앱 수명에만 의존한다. 구체 전송은 팩토리/주입으로 조립.
public sealed class HelixServer : IAsyncDisposable
{
	private readonly IServerTransport _transport;
	private readonly CancellationTokenSource _stop = new();
	private readonly TaskCompletionSource _finished = new(TaskCreationOptions.RunContinuationsAsynchronously);
	private int _started;
	public int Port => _transport.Port;
	public int SessionCount => _transport.SessionCount;
	public Exception? LastError => _transport.LastError;

	public HelixServer(EngineConfig config, IServerTransport? transport = null)
	{
		ArgumentNullException.ThrowIfNull(config);
		config.Validate();
		_transport = transport ?? TransportFactory.Create(config);
	}
	public static HelixServer Create(string path) => new(EngineConfig.Load(path));
	public Session? FindSession(ulong id) => _transport.FindSession(id);
	public bool RouteToSession(ulong id, ReadOnlySpan<byte> message) => FindSession(id)?.Send(message) ?? false;
	public void Stop() => _stop.Cancel();

	public async Task RunAsync(IHelixApp app, CancellationToken cancellation = default)
	{
		ArgumentNullException.ThrowIfNull(app);
		if (Interlocked.CompareExchange(ref _started, 1, 0) != 0) throw new InvalidOperationException("Server instances run once.");
		using var linked = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, cancellation);
		var appStarted = false;
		try
		{
			_transport.Start();
			appStarted = true;
			app.OnStart(this);
			await _transport.RunAsync(app, linked.Token).ConfigureAwait(false);
		}
		finally
		{
			try
			{
				await _transport.DisposeAsync().ConfigureAwait(false);
				if (appStarted) app.OnStop();
			}
			finally { _finished.TrySetResult(); }
		}
	}

	public async ValueTask DisposeAsync()
	{
		Stop();
		if (Volatile.Read(ref _started) != 0) await _finished.Task.ConfigureAwait(false);
		else await _transport.DisposeAsync().ConfigureAwait(false);
	}
}
