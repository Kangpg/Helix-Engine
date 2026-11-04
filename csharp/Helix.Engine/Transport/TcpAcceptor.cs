using System.Net;
using System.Net.Sockets;

namespace Helix.Engine.Transport;

internal sealed class TcpAcceptor(EngineConfig config) : IDisposable
{
	private readonly TcpListener _listener = new(IPAddress.Parse(config.Ip), config.Port);
	public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;
	public void Start() => _listener.Start(config.Backlog);
	public ValueTask<TcpClient> AcceptAsync(CancellationToken cancellation) => _listener.AcceptTcpClientAsync(cancellation);
	public void Dispose() => _listener.Stop();
}
