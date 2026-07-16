$orig = "pipeline/test_blk_9_attn_q_weight.bin"
$dec = "pipeline/test_blk_9_attn_q_weight_dec.bin"
$enc = "pipeline/test_blk_9_attn_q_weight.gpx5"
if ((Test-Path $orig) -and (Test-Path $dec)) {
    $o = Get-Item $orig
    $d = Get-Item $dec
    $e = Get-Item $enc
    "Original: $($o.Length) bytes"
    "Decoded:  $($d.Length) bytes"
    "Encoded:  $($e.Length) bytes"
    $ob = [System.IO.File]::ReadAllBytes($orig)
    $db = [System.IO.File]::ReadAllBytes($dec)
    if ($d.Length -ne $o.Length) {
        "SIZE MISMATCH: orig=$($o.Length) dec=$($d.Length)"
    }
    $diff = 0
    for ($i = 0; $i -lt [Math]::Min($ob.Length, $db.Length); $i++) {
        if ($ob[$i] -ne $db[$i]) { 
            $diff++
            if ($diff -le 10) {
                $oh = ("{0:x2}" -f $ob[$i])
                $dh = ("{0:x2}" -f $db[$i])
                "Diff at byte $($i): orig=0x$oh dec=0x$dh"
            }
        }
    }
    "Total differing bytes: $diff out of $($ob.Length)"
} else {
    "Files not found"
}
