using System.Collections.Concurrent;
using System.Net.Sockets;

namespace Helix.Engine.Transport;

// 조립/실행 수명만 소유한다. 수락·프레이밍·송신 큐·세션은 각 클래스에 위임.
internal sealed class TcpServerTransport : IServerTransport
{
	private readonly EngineConfig _config;
	private readonly ConcurrentDictionary<ulong, TcpSession> _sessions = new();
	private readonly CancellationTokenSource _stop = new();
	private readonly TaskCompletionSource _finished = new(TaskCreationOptions.RunContinuationsAsynchronously);
	private long _sequence;
	private int _started;
	private int _runStarted;
	private TcpAcceptor? _acceptor;
	private int _port;
	public int Port => Volatile.Read(ref _port);
	public int SessionCount => _sessions.Count;
	public Exception? LastError { get; private set; }

	public TcpServerTransport(EngineConfig config)
	{
		_config = config ?? throw new ArgumentNullException(nameof(config));
		_config.Validate();
		if (_config.ClusterEnabled) throw new NotSupportedException("Managed Redis backplane is not implemented yet.");
	}
	public Session? FindSession(ulong id) => _sessions.TryGetValue(id, out var session) ? session.Handle : null;
	public void Start()
	{
		if (Interlocked.CompareExchange(ref _started, 1, 0) != 0) throw new InvalidOperationException("Transport runs once.");
		_acceptor = new TcpAcceptor(_config);
		_acceptor.Start();
		Volatile.Write(ref _port, _acceptor.Port);
	}

	public async Task RunAsync(IHelixApp app, CancellationToken cancellation = default)
	{
		ArgumentNullException.ThrowIfNull(app);
		if (_acceptor is null || Interlocked.CompareExchange(ref _runStarted, 1, 0) != 0) throw new InvalidOperationException("Transport must be started once before Run.");
		using var linked = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, cancellation);
		var acceptor = _acceptor;
		try
		{
			while (!linked.IsCancellationRequested)
			{
				var client = await acceptor.AcceptAsync(linked.Token).ConfigureAwait(false);
				if (_sessions.Count >= _config.MaxConnections) { client.Dispose(); continue; }
				var id = (ulong)Interlocked.Increment(ref _sequence);
				var session = new TcpSession(id, client, _config);
				_sessions[id] = session;
				_ = Task.Run(() => session.RunAsync(app, error => LastError = error, () => _sessions.TryRemove(id, out _)));
			}
		}
		catch (OperationCanceledException) when (linked.IsCancellationRequested) { }
		catch (SocketException) when (linked.IsCancellationRequested) { }
		finally
		{
			acceptor.Dispose();
			var remaining = _sessions.Values.ToArray();
			foreach (var session in remaining) session.Close(CloseReason.ServerShutdown);
			try
			{
				await Task.WhenAll(remaining.Select(s => s.Completion)).WaitAsync(TimeSpan.FromMilliseconds(_config.DrainTimeoutMs)).ConfigureAwait(false);
			}
			finally { _finished.TrySetResult(); }
		}
	}

	public async ValueTask DisposeAsync()
	{
		_stop.Cancel();
		_acceptor?.Dispose();
		if (Volatile.Read(ref _runStarted) != 0) await _finished.Task.ConfigureAwait(false);
	}
}
