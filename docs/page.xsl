<?xml version="1.0"?>
<xsl:stylesheet
  xmlns="http://www.w3.org/1999/xhtml"
  xmlns:html="http://www.w3.org/1999/xhtml"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:exsl="http://exslt.org/common"
  exclude-result-prefixes="xsl exsl html"
  version="1.0">

  <xsl:param name="builddir" select="'..'"/>

  <xsl:template match="node() | @*" mode="content">
    <xsl:copy>
      <xsl:apply-templates select="node() | @*" mode="content"/>
    </xsl:copy>
  </xsl:template>

  <xsl:template match="html:div[@id='include']" mode="content">
    <xsl:call-template name="include"/>
  </xsl:template>

  <!-- This is the master page structure -->
  <xsl:template match="/" mode="page">
    <xsl:param name="pagesrc"/>
    <xsl:param name="timestamp"/>
    <xsl:text disable-output-escaping="yes">&lt;!DOCTYPE html&gt;
</xsl:text>
    <html data-sourcedoc="{$pagesrc}">
      <xsl:comment>
        This file is autogenerated from <xsl:value-of select="$pagesrc"/>
        Do not edit this file. Changes will be lost.
      </xsl:comment>
      <xsl:comment>
        This page was generated at <xsl:value-of select="$timestamp"/>.
      </xsl:comment>
      <head>
        <meta charset="UTF-8"/>
        <meta name="viewport" content="width=device-width, initial-scale=1"/>
        <link rel="stylesheet" type="text/css" href="{$href_base}css/main.css"/>
        <link rel="apple-touch-icon" sizes="180x180" href="/apple-touch-icon.png"/>
        <link rel="icon" type="image/png" sizes="32x32" href="/favicon-32x32.png"/>
        <link rel="icon" type="image/png" sizes="16x16" href="/favicon-16x16.png"/>
        <link rel="manifest" href="/manifest.json"/>
        <meta name="theme-color" content="#ffffff"/>
        <title>libvirt: <xsl:value-of select="html:html/html:body//html:h1"/></title>
        <meta name="description" content="libvirt, virtualization, virtualization API"/>
        <xsl:if test="$pagesrc = 'docs/libvirt-go.rst'">
          <meta name="go-import" content="libvirt.org/libvirt-go git https://gitlab.com/libvirt/libvirt-go.git"/>
        </xsl:if>
        <xsl:if test="$pagesrc = 'docs/libvirt-go-xml.rst'">
          <meta name="go-import" content="libvirt.org/libvirt-go-xml git https://gitlab.com/libvirt/libvirt-go-xml.git"/>
        </xsl:if>
        <xsl:if test="$pagesrc = 'docs/go/libvirt.rst'">
          <meta name="go-import" content="libvirt.org/go/libvirt git https://gitlab.com/libvirt/libvirt-go-module.git"/>
        </xsl:if>
        <xsl:if test="$pagesrc = 'docs/go/libvirtxml.rst'">
          <meta name="go-import" content="libvirt.org/go/libvirtxml git https://gitlab.com/libvirt/libvirt-go-xml-module.git"/>
        </xsl:if>
        <xsl:apply-templates select="/html:html/html:head/html:script" mode="content"/>

        <script type="text/javascript" src="{$href_base}js/main.js">
          <xsl:comment>// forces non-empty element</xsl:comment>
        </script>
      </head>
      <body onload="pageload()">
        <div id="body">
          <xsl:choose>
            <!-- docutils-0.16 and older use a div as container for contents -->
            <xsl:when test="html:html/html:body/html:div/@class='document'">
              <xsl:apply-templates select="/html:html/html:body/*" mode="content"/>
            </xsl:when>
            <!-- docutils-0.17 adopted use of the 'main' semantic container -->
            <xsl:when test="html:html/html:body/html:main">
              <xsl:apply-templates select="/html:html/html:body/*" mode="content"/>
            </xsl:when>
          </xsl:choose>
        </div>
        <div id="nav">
          <div id="home">
            <a href="{$href_base}index.html">Home</a>
          </div>
          <div id="jumplinks">
            <ul>
              <li><a href="{$href_base}downloads.html">Download</a></li>
              <li><a href="{$href_base}contribute.html">Contribute</a></li>
              <li><a href="{$href_base}docs.html">Docs</a></li>
            </ul>
          </div>
          <div id="search">
            <form id="simplesearch" action="https://www.google.com/search" enctype="application/x-www-form-urlencoded" method="get">
              <div>
                <input id="searchsite" name="sitesearch" type="hidden" value="libvirt.org"/>
                <input id="searchq" name="q" type="text" size="12" value=""/>
                <input name="submit" type="submit" value="Go"/>
              </div>
            </form>
            <div id="advancedsearch">
              <span><input type="radio" name="what" id="whatwebsite" checked="checked" value="website"/><label for="whatwebsite">Website</label></span>
              <span><input type="radio" name="what" id="whatwiki" value="wiki"/><label for="whatwiki">Wiki</label></span>
              <span><input type="radio" name="what" id="whatdevs" value="devs"/><label for="whatdevs">Developers list</label></span>
              <span><input type="radio" name="what" id="whatusers" value="users"/><label for="whatusers">Users list</label></span>
            </div>
          </div>
        </div>
        <div id="footer">
          <div id="contact">
            <h3>Contact</h3>
            <ul>
              <li><a href="{$href_base}contact.html#mailing-lists">email</a></li>
              <li><a href="{$href_base}contact.html#irc">irc</a></li>
            </ul>
          </div>
          <div id="community">
            <h3>Community</h3>
            <ul>
              <li><a href="https://fosstodon.org/tags/libvirt">fosstodon</a></li>
              <li><a href="https://stackoverflow.com/questions/tagged/libvirt">stackoverflow</a></li>
              <li><a href="https://serverfault.com/questions/tagged/libvirt">serverfault</a></li>
            </ul>
          </div>
          <xsl:if test="$pagesrc != ''">
            <div id="contribute">
              <h3>Contribute</h3>
              <ul>
                <li><a href="https://gitlab.com/libvirt/libvirt/-/blob/master/{$pagesrc}">edit this page</a></li>
              </ul>
            </div>
          </xsl:if>
          <div id="conduct">
            Participants in the libvirt project agree to abide by <a href="{$href_base}governance.html#code-of-conduct">the project code of conduct</a>
          </div>
          <br class="clear"/>
        </div>
      </body>
    </html>
  </xsl:template>

  <xsl:template name="include">
    <xsl:variable name="inchtml">
      <xsl:copy-of select="document(concat($builddir, '/docs/', @filename))"/>
    </xsl:variable>

    <xsl:apply-templates select="exsl:node-set($inchtml)/html:html/html:body/*" mode="content"/>
  </xsl:template>

  <xsl:template match="html:h1 | html:h2 | html:h3 | html:h4 | html:h5 | html:h6" mode="content">
    <xsl:element name="{name()}">
      <xsl:apply-templates mode="copy" />
      <xsl:if test="./html:a/@id">
        <a class="headerlink" href="#{html:a/@id}" title="Link to this headline">&#xb6;</a>
      </xsl:if>
      <xsl:if test="parent::html:div[@class='section']">
        <a class="headerlink" href="#{../@id}" title="Link to this headline">&#xb6;</a>
      </xsl:if>
    </xsl:element>
  </xsl:template>

  <xsl:template match="text()" mode="copy" priority="0">
    <xsl:value-of select="."/>
  </xsl:template>

  <xsl:template match="*" mode="copy">
    <xsl:element name="{name()}">
      <xsl:copy-of select="./@*"/>
      <xsl:apply-templates mode="copy" />
    </xsl:element>
  </xsl:template>
</xsl:stylesheet>
