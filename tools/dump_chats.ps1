$xml = [xml](Get-Content (Join-Path $PSScriptRoot 'wa6.chats.plist'))
$dict = $xml.plist.dict
$list = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $dict.ChildNodes.Count; $i += 2) {
    $k = $dict.ChildNodes[$i].InnerText
    if ($k.StartsWith('_')) { continue }
    $v = $dict.ChildNodes[$i+1]
    $jidNode = $v.SelectSingleNode('key[text()="jid"]/following-sibling::string[1]')
    $nameNode = $v.SelectSingleNode('key[text()="name"]/following-sibling::string[1]')
    $pushNode = $v.SelectSingleNode('key[text()="pushName"]/following-sibling::string[1]')
    $pnNode = $v.SelectSingleNode('key[text()="pnJid"]/following-sibling::string[1]')
    $customNode = $v.SelectSingleNode('key[text()="customName"]/following-sibling::string[1]')

    $jid = if ($jidNode) { $jidNode.InnerText } else { '' }
    $name = if ($nameNode) { $nameNode.InnerText } else { '' }
    $push = if ($pushNode) { $pushNode.InnerText } else { '' }
    $pn = if ($pnNode) { $pnNode.InnerText } else { '' }
    $custom = if ($customNode) { $customNode.InnerText } else { '' }

    [void]$list.Add([PSCustomObject]@{
        Key = $k
        JID = $jid
        Name = $name
        PushName = $push
        pnJid = $pn
        customName = $custom
    })
}
$list | Format-Table -AutoSize
