namespace Helix.Engine.Transport;

// 전송 구현은 콜백 순서, 송신 직렬화, 종료 시 진행 I/O 회수 계약을 지킨다.
public interface IServerTransport : IAsyncDisposable
{
	int Port { get; }
	int SessionCount { get; }
	Exception? LastError { get; }
	void Start();
	Task RunAsync(IHelixApp app, CancellationToken cancellation);
	Session? FindSession(ulong id);
}
