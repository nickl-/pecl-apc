<?php
/* based on sapi/cli/tests/php_cli_server.inc */
define ("PHP_CLI_SERVER_HOSTNAME", "localhost");
define ("PHP_CLI_SERVER_PORT", 8964);
define ("PHP_CLI_SERVER_ADDRESS", PHP_CLI_SERVER_HOSTNAME.":".PHP_CLI_SERVER_PORT);

function server_start($code = 'echo "Hello world";', $php_opts = array(), $no_router = FALSE)
{
	$php_executable = getenv('TEST_PHP_EXECUTABLE');
	$doc_root = __DIR__;
	$router = "index.php";

	if ($code) {
		file_put_contents($doc_root . '/' . $router, '<?php ' . $code . ' ?>');
	}

	$descriptorspec = array(
		0 => STDIN,
		1 => STDOUT,
		2 => STDERR,
	);

	$ext = (substr(PHP_OS, 0, 3) == 'WIN') ? 'php_apc.dll' : 'apc.so';
	$php_args = "-d extension_dir=$doc_root/../modules -d extension=$ext";
	if ($php_opts) {
		$php_args = "$php_args -d " . implode(' -d ', $php_opts);;
	}

	if (substr(PHP_OS, 0, 3) == 'WIN') {
		$cmd = "{$php_executable} -n $php_args -t {$doc_root} -S " . PHP_CLI_SERVER_ADDRESS;
		if (!$no_router) {
			$cmd .= " {$router}";
		}

		$handle = proc_open(addslashes($cmd), $descriptorspec, $pipes, $doc_root, NULL, array("bypass_shell" => true,  "suppress_errors" => true));
	} else {
		$cmd = "exec {$php_executable} -n $php_args -t {$doc_root} -S " . PHP_CLI_SERVER_ADDRESS;
		if (!$no_router) {
			$cmd .= " {$doc_root}/{$router}";
		}
		$cmd .= " 2>/dev/null";

		$handle = proc_open($cmd, $descriptorspec, $pipes, $doc_root);
	}
	
	// note: even when server prints 'Listening on localhost:8964...Press Ctrl-C to quit.'
	//       it might not be listening yet...need to wait until fsockopen() call returns
    $i = 0;
    while (($i++ < 5) && !($fp = @fsockopen(PHP_CLI_SERVER_HOSTNAME, PHP_CLI_SERVER_PORT))) {
        usleep(10000);
    }

    if ($fp) {
        fclose($fp);
    }

	register_shutdown_function(
		function($handle) use($router) {
			proc_terminate($handle);
			@unlink(__DIR__ . "/{$router}");
		},
			$handle
		);
	// don't bother sleeping, server is already up
	// server can take a variable amount of time to be up, so just sleeping a guessed amount of time
	// does not work. this is why tests sometimes pass and sometimes fail. to get a reliable pass
	// sleeping doesn't work.
}

function get_response($fp, $data_only = true)
{
	$s = '';

	while (!feof($fp)) {
		$s .= fgets($fp);
	}

	if ($data_only) {
		$parts = explode("\r\n\r\n", $s);
		$s = $parts[1];
	}

	return $s;
}


function run_test_simple()
{
	list($host, $port) = explode(':', PHP_CLI_SERVER_ADDRESS);
	$port = intval($port)?:80;

	$fp = fsockopen($host, $port, $errno, $errstr, 0.5);
	if (!$fp) {
	  die(sprintf("connect failed errno=%d errstr='%s'", $errno, $errstr));
	}

	$send = "GET / HTTP/1.1\nHost: {$host}\r\n\r\n";

	if(fwrite($fp, $send)) {
		echo get_response($fp);
	}
}

function run_test($host, $port, $send)
{
	$fp = fsockopen($host, $port, $errno, $errstr, 0.5);
	if (!$fp) {
	  die("connect failed");
	}

	if(fwrite($fp, $send)) {
		echo get_response($fp);
	}
}