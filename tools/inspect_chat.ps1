param(
    [string]$Key = "90297560260800",
    [string]$Path = (Join-Path $PSScriptRoot 'wa6.chats.plist')
)
$xml = [xml](Get-Content $Path)
$dict = $xml.plist.dict
for ($i = 0; $i -lt $dict.ChildNodes.Count; $i += 2) {
    if ($dict.ChildNodes[$i].InnerText -eq $Key) {
        $v = $dict.ChildNodes[$i+1]
        $msgsNode = $v.SelectSingleNode('key[text()="msgs"]/following-sibling::array[1]')
        if ($msgsNode) {
            $dicts = $msgsNode.SelectNodes('dict')
            Write-Host "Messages count for $Key : $($dicts.Count)"
            foreach ($d in $dicts) {
                $txt = $d.SelectSingleNode('key[text()="display"]/following-sibling::string[1]')
                if (-not $txt) { $txt = $d.SelectSingleNode('key[text()="text"]/following-sibling::string[1]') }
                $out = $d.SelectSingleNode('key[text()="out"]/following-sibling::*[1]')
                $kind = $d.SelectSingleNode('key[text()="kind"]/following-sibling::integer[1]')
                $mid = $d.SelectSingleNode('key[text()="id"]/following-sibling::string[1]')
                $status = $d.SelectSingleNode('key[text()="status"]/following-sibling::integer[1]')
                $t = if ($txt) { $txt.InnerText } else { '' }
                $o = if ($out) { $out.Name } else { '' }
                $k = if ($kind) { $kind.InnerText } else { '0' }
                $m = if ($mid) { $mid.InnerText } else { '' }
                $s = if ($status) { $status.InnerText } else { '' }
                Write-Host "[$o k=$k id=$m st=$s] $t"
            }
        } else {
            Write-Host "No msgs node for $Key"
        }
        break
    }
}
